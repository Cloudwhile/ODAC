/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "platform.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

int odac_platform_valid_interface(const char *s)
{
    size_t n = s ? strlen(s) : 0;
    if (!n || n > 15 || !isalnum((unsigned char)s[0])) return 0;
    for (size_t i = 0; i < n; i++)
        if (!isalnum((unsigned char)s[i]) && s[i] != '_' && s[i] != '-' && s[i] != '.') return 0;
    return strcmp(s, "lo") != 0;
}
int odac_platform_valid_mac(const char *s)
{
    if (!s || strlen(s) != 17) return 0;
    for (int i = 0; i < 17; i++)
        if (i % 3 == 2 ? s[i] != ':' : !isxdigit((unsigned char)s[i])) return 0;
    unsigned first = (unsigned)strtoul(s, NULL, 16);
    return !(first & 1) && strcasecmp(s, "00:00:00:00:00:00") != 0;
}
int odac_platform_valid_ipv4(const char *s)
{
    struct in_addr a;
    if (!s || inet_pton(AF_INET, s, &a) != 1) return 0;
    uint32_t v = ntohl(a.s_addr);
    return v && (v >> 24) != 127 && (v >> 24) != 0 && (v >> 24) < 224;
}
static int64_t milliseconds(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return -1;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
int odac_platform_run(const char *const argv[], char *out, size_t cap, int timeout_ms)
{
    int fd[2], status = 0, eof = 0, done = 0, failed = 0;
    size_t used = 0;
    if (!argv || !argv[0] || argv[0][0] != '/' || !out || cap < 2 || timeout_ms < 1) return -1;
    out[0] = 0;
    if (pipe2(fd, O_CLOEXEC)) return -1;
    int64_t deadline = milliseconds() + timeout_ms;
    pid_t pid = fork();
    if (pid < 0) { close(fd[0]); close(fd[1]); return -1; }
    if (!pid) {
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        signal(SIGPIPE, SIG_DFL);
        setpgid(0, 0);
        int nullfd = open("/dev/null", O_RDWR);
        if (nullfd < 0 || dup2(nullfd, STDIN_FILENO) < 0 || dup2(fd[1], STDOUT_FILENO) < 0 || dup2(nullfd, STDERR_FILENO) < 0) _exit(126);
#ifdef SYS_close_range
        if (syscall(SYS_close_range, 3U, ~0U, 0) < 0)
#endif
        {
            struct rlimit lim;
            if (getrlimit(RLIMIT_NOFILE, &lim)) _exit(126);
            for (rlim_t i = 3; i < lim.rlim_cur; i++) close((int)i);
        }
        char *const env[] = {"PATH=/usr/sbin:/usr/bin:/sbin:/bin", "LC_ALL=C", NULL};
        execve(argv[0], (char *const *)argv, env);
        _exit(127);
    }
    close(fd[1]);
    (void)setpgid(pid, pid);
    if (fcntl(fd[0], F_SETFL, O_NONBLOCK) < 0) failed = 1;
    while (!failed && !(eof && done)) {
        int64_t left = deadline - milliseconds();
        if (left <= 0) { failed = 1; break; }
        struct pollfd p = {fd[0], POLLIN | POLLHUP, 0};
        int n = poll(eof ? NULL : &p, eof ? 0 : 1, (int)(left > 25 ? 25 : left));
        if (n < 0 && errno != EINTR) { failed = 1; break; }
        if (!eof && n > 0) {
            char chunk[4096];
            ssize_t got;
            while ((got = read(fd[0], chunk, sizeof(chunk))) > 0) {
                if ((size_t)got >= cap - used) { failed = 1; break; }
                memcpy(out + used, chunk, (size_t)got); used += (size_t)got;
            }
            if (!got) eof = 1;
            else if (got < 0 && errno != EAGAIN && errno != EINTR) failed = 1;
        }
        if (!done) {
            pid_t w = waitpid(pid, &status, WNOHANG);
            if (w == pid) done = 1;
            else if (w < 0 && errno != EINTR) failed = 1;
        }
    }
    if (failed) {
        kill(-pid, SIGKILL);
        if (!done) { kill(pid, SIGKILL); while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {} }
    }
    close(fd[0]); out[used] = 0;
    /* Exit status 0..125 is returned for callers with documented codes; exec,
     * timeout, signal and overflow failures are always -1. */
    if (failed || !WIFEXITED(status) || WEXITSTATUS(status) > 125) return -1;
    return WEXITSTATUS(status);
}
const char *odac_platform_string(struct json_object *o, const char *key)
{
    struct json_object *v;
    if (!o || !json_object_object_get_ex(o, key, &v) || !json_object_is_type(v, json_type_string)) return NULL;
    const char *s = json_object_get_string(v);
    return strlen(s) == (size_t)json_object_get_string_len(v) ? s : NULL;
}
struct json_object *odac_platform_parse_json(const char *s)
{
    if (!s || strlen(s) >= ODAC_PLATFORM_OUTPUT) return NULL;
    struct json_tokener *tok = json_tokener_new_ex(16);
    if (!tok) return NULL;
    json_tokener_set_flags(tok, JSON_TOKENER_STRICT);
    size_t n = strlen(s);
    struct json_object *o = json_tokener_parse_ex(tok, s, (int)n);
    size_t end = json_tokener_get_parse_end(tok);
    int ok = json_tokener_get_error(tok) == json_tokener_success;
    while (end < n && isspace((unsigned char)s[end])) end++;
    json_tokener_free(tok);
    if (!ok || end != n || !json_object_is_type(o, json_type_object)) { if (o) json_object_put(o); return NULL; }
    return o;
}
static int matches(struct json_object *o, const char *ip, const char *mac)
{
    const char *i = odac_platform_string(o, "ip"), *m = odac_platform_string(o, "mac"), *s = odac_platform_string(o, "state");
    return odac_platform_valid_ipv4(i) && odac_platform_valid_mac(m) && s &&
        (!strcmp(s, "Authenticated") || !strcmp(s, "Preauthenticated")) &&
        (!ip || !strcmp(ip, i)) && (!mac || !strcasecmp(mac, m));
}
struct json_object *odac_platform_parse_client(struct json_object *root, const char *ip, const char *mac)
{
    if ((!ip && !mac) || (ip && !odac_platform_valid_ipv4(ip)) || (mac && !odac_platform_valid_mac(mac))) return NULL;
    struct json_object *clients, *found = NULL;
    if (!json_object_object_get_ex(root, "clients", &clients)) return matches(root, ip, mac) ? json_object_get(root) : NULL;
    if (!json_object_is_type(clients, json_type_object) || json_object_object_length(clients) > 4096) return NULL;
    json_object_object_foreach(clients, key, value) {
        const char *m = odac_platform_string(value, "mac");
        if (!m || strcasecmp(m, key)) return NULL;
        if (matches(value, ip, mac)) { if (found) return NULL; found = value; }
    }
    return found ? json_object_get(found) : NULL;
}
int odac_platform_neighbor_matches(const char *text, const char *iface, const char *ip, const char *mac)
{
    if (!text || !odac_platform_valid_interface(iface) || !odac_platform_valid_ipv4(ip) || !odac_platform_valid_mac(mac)) return 0;
    char *copy = strdup(text), *save, *line;
    int matches_count = 0;
    if (!copy) return 0;
    for (line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char addr[64], dev[32], hw[32], state[32], extra[32];
        int n = sscanf(line, "%63s dev %31s lladdr %31s %31s %31s", addr, dev, hw, state, extra);
        if (n < 1 || strcmp(addr, ip)) continue;
        if (n != 4 || strcmp(dev, iface) || strcasecmp(hw, mac) ||
            (strcmp(state, "REACHABLE") && strcmp(state, "STALE") && strcmp(state, "DELAY") && strcmp(state, "PROBE") && strcmp(state, "PERMANENT"))) { matches_count = -1; break; }
        matches_count++;
    }
    free(copy);
    return matches_count == 1;
}
/* Unique IPv4 for MAC on the interface, whose IP is itself unambiguously bound. */
int odac_platform_neighbor_ip(const char *text, const char *iface, const char *mac, char ip[16])
{
    ip[0] = 0;
    if (!text || !odac_platform_valid_interface(iface) || !odac_platform_valid_mac(mac)) return -1;
    char *copy = strdup(text), *save, *line, found[16] = "";
    int count = 0;
    if (!copy) return -1;
    for (line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char addr[64], dev[32], hw[32], state[32];
        if (sscanf(line, "%63s dev %31s lladdr %31s %31s", addr, dev, hw, state) != 4 || strcasecmp(hw, mac) || strcmp(dev, iface)) continue;
        if (++count > 1 || strlen(addr) >= sizeof(found)) break;
        snprintf(found, sizeof(found), "%s", addr);
    }
    free(copy);
    if (count != 1 || !odac_platform_neighbor_matches(text, iface, found, mac)) return -1;
    memcpy(ip, found, sizeof(found));
    return 0;
}
