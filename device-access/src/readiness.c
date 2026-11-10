#define _GNU_SOURCE
#include "transport.h"
#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/inotify.h>
#include <time.h>
#include <unistd.h>

struct json_object *odac_wait_ready(const char *path, struct json_object *request)
{
    char directory[256];
    if (strlen(path) >= sizeof(directory)) return NULL;
    strcpy(directory, path);
    char *slash = strrchr(directory, '/');
    if (!slash) return NULL;
    *slash = '\0';
    int watcher = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (watcher < 0) return NULL;
    if (inotify_add_watch(watcher, directory, IN_CREATE | IN_MOVED_TO | IN_ATTRIB) < 0) {
        close(watcher);
        return NULL;
    }
    struct timespec start, now;
    if (clock_gettime(CLOCK_MONOTONIC, &start)) { close(watcher); return NULL; }
    struct json_object *reply = NULL;
    for (;;) {
        if (clock_gettime(CLOCK_MONOTONIC, &now)) break;
        int64_t remaining_ms = 20000 - (now.tv_sec - start.tv_sec) * 1000 -
                               (now.tv_nsec - start.tv_nsec) / 1000000;
        if (remaining_ms <= 0) break;
        reply = odac_exchange_deadline(path, request, remaining_ms);
        if (reply) break;
        if (clock_gettime(CLOCK_MONOTONIC, &now)) break;
        remaining_ms = 20000 - (now.tv_sec - start.tv_sec) * 1000 -
                       (now.tv_nsec - start.tv_nsec) / 1000000;
        if (remaining_ms <= 0) break;
        struct pollfd ready = { .fd = watcher, .events = POLLIN };
        int status = poll(&ready, 1, (int)remaining_ms);
        if (status < 0 && errno == EINTR) continue;
        if (status <= 0) break;
        char events[4096];
        ssize_t consumed = read(watcher, events, sizeof(events));
        if (consumed < 0 && errno != EAGAIN && errno != EINTR) break;
    }
    close(watcher);
    return reply;
}
