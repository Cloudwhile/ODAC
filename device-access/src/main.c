#define _GNU_SOURCE
#include "config.h"
#include "transport.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static volatile sig_atomic_t running = 1;
static int peer_is_privileged(int fd)
{
    struct ucred credentials;
    socklen_t length = sizeof(credentials);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length)) return 0;
    return credentials.uid == 0;
}

static void handle_signal(int signal_number) { running = 0; (void)signal_number; }

static int create_socket(const char *path)
{
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof(address.sun_path)) return -1;
    memcpy(address.sun_path, path, strlen(path) + 1);
    unlink(path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    mode_t previous = umask(0077);
    int bound = bind(fd, (struct sockaddr *)&address, sizeof(address));
    umask(previous);
    if (bound) { close(fd); return -1; }
    if (listen(fd, 16) || chmod(path, 0600)) { close(fd); unlink(path); return -1; }
    return fd;
}

static void serve_client(struct odac *ctx, int fd)
{
    struct json_object *request = odac_receive(fd, ODAC_MESSAGE_MAX, 0);
    if (!request) return;
    struct json_object *reply = odac_dispatch(ctx, request);
    json_object_put(request);
    if (!reply) return;
    odac_send(fd, reply, 0);
    json_object_put(reply);
}

int main(int argc, char **argv)
{
    if (argc != 3 || strcmp(argv[1], "--config")) {
        fprintf(stderr, "usage: odacd --config /path/config.json\n");
        return 2;
    }
    struct odac ctx = { 0 };
    if (odac_config_load(&ctx.config, argv[2])) {
        fprintf(stderr, "odacd: configuration rejected\n");
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);
    signal(SIGHUP, handle_signal);
    if (odac_platform_init(&ctx)) {
        odac_platform_gate(&ctx, 0);
        fprintf(stderr, "odacd: platform initialization failed, network stays closed\n");
        return 1;
    }
    if (odac_open(&ctx)) {
        odac_platform_gate(&ctx, 0);
        odac_platform_close(&ctx);
        fprintf(stderr, "odacd: database unavailable, network stays closed\n");
        return 1;
    }
    odac_tick(&ctx, time(NULL));
    int server = create_socket(ctx.config.socket_path);
    if (server < 0) {
        odac_platform_gate(&ctx, 0);
        odac_close(&ctx);
        odac_platform_close(&ctx);
        fprintf(stderr, "odacd: control socket unavailable\n");
        return 1;
    }
    time_t next_tick = time(NULL) + 5;
    while (running) {
        struct pollfd ready = { .fd = server, .events = POLLIN };
        int status = poll(&ready, 1, 1000);
        if (status < 0 && errno == EINTR) continue;
        if (status > 0 && (ready.revents & POLLIN)) {
            int client = accept(server, NULL, NULL);
            if (client >= 0) {
                if (peer_is_privileged(client)) serve_client(&ctx, client);
                close(client);
            }
        }
        time_t now = time(NULL);
        if (now >= next_tick) {
            odac_tick(&ctx, now);
            next_tick = now + 5;
        }
    }
    close(server);
    unlink(ctx.config.socket_path);
    odac_platform_gate(&ctx, 0);
    odac_close(&ctx);
    odac_platform_close(&ctx);
    return 0;
}
