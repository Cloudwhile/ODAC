#define _GNU_SOURCE
#include "transport.h"
#include "json_text.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

struct json_object *odac_failure(const char *code, const char *message)
{
    struct json_object *reply = json_object_new_object();
    struct json_object *error = json_object_new_object();
    json_object_object_add(reply, "ok", json_object_new_boolean(0));
    json_object_object_add(error, "code", json_object_new_string(code));
    json_object_object_add(error, "message", json_object_new_string(message));
    json_object_object_add(reply, "error", error);
    return reply;
}

struct json_object *odac_json_parse(const char *text, size_t length, size_t limit)
{
    if (length == 0 || length > limit) return NULL;
    return odac_json_parse_strict(text, length, 16);
}

static int64_t milliseconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

struct json_object *odac_receive(int fd, size_t limit, int64_t deadline)
{
    char *buffer = malloc(limit + 1);
    if (!buffer) return NULL;
    size_t used = 0;
    if (deadline <= 0) deadline = milliseconds() + 10000;
    struct json_object *result = NULL;
    while (used < limit + 1) {
        int64_t remaining = deadline - milliseconds();
        if (remaining <= 0) break;
        struct pollfd ready = { .fd = fd, .events = POLLIN };
        int status = poll(&ready, 1, (int)remaining);
        if (status < 0 && errno == EINTR) continue;
        if (status <= 0) break;
        ssize_t count = read(fd, buffer + used, limit + 1 - used);
        if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (count <= 0) break;
        used += (size_t)count;
        char *end = memchr(buffer, '\n', used);
        if (end) {
            size_t length = (size_t)(end - buffer);
            if (length + 1 == used) result = odac_json_parse(buffer, length, limit);
            break;
        }
    }
    explicit_bzero(buffer, used);
    free(buffer);
    return result;
}

int odac_send(int fd, struct json_object *value, int64_t deadline)
{
    const char *text = json_object_to_json_string_ext(value, JSON_C_TO_STRING_PLAIN);
    size_t length = strlen(text);
    if (length > ODAC_REPLY_MAX) return -1;
    size_t sent = 0;
    if (deadline <= 0) deadline = milliseconds() + 10000;
    while (sent < length + 1) {
        int64_t remaining = deadline - milliseconds();
        if (remaining <= 0) return -1;
        struct pollfd ready = { .fd = fd, .events = POLLOUT };
        int status = poll(&ready, 1, (int)remaining);
        if (status < 0 && errno == EINTR) continue;
        if (status <= 0) return -1;
        const char *start = sent < length ? text + sent : "\n";
        size_t amount = sent < length ? length - sent : 1;
        ssize_t count = send(fd, start, amount, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (count <= 0) return -1;
        sent += (size_t)count;
    }
    return 0;
}

struct json_object *odac_exchange_deadline(const char *path, struct json_object *request, int64_t budget_ms)
{
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof(address.sun_path)) return NULL;
    memcpy(address.sun_path, path, strlen(path) + 1);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return NULL;
    int64_t deadline = milliseconds() + budget_ms;
    struct json_object *reply = NULL;
    int done = 0;
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0) {
        done = 1;
    } else if (errno == EINPROGRESS) {
        for (;;) {
            int64_t remaining = deadline - milliseconds();
            if (remaining <= 0) break;
            struct pollfd ready = { .fd = fd, .events = POLLOUT };
            int status = poll(&ready, 1, (int)remaining);
            if (status < 0 && errno == EINTR) continue;
            if (status <= 0) break;
            int error = 0;
            socklen_t length = sizeof(error);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) || error) break;
            done = 1;
            break;
        }
    }
    if (done) {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
        if (odac_send(fd, request, deadline) == 0) reply = odac_receive(fd, ODAC_REPLY_MAX, deadline);
    }
    close(fd);
    return reply;
}

struct json_object *odac_exchange(const char *path, struct json_object *request)
{
    return odac_exchange_deadline(path, request, 10000);
}
