/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "platform.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef ODAC_STATUS_OWNER
#define ODAC_STATUS_OWNER 0
#endif

/* WAN state comes from the administrator's own login script.
 *
 * The script writes one status file in a fixed format and ODAC only reads it:
 *
 *   state=ONLINE
 *   updated=1757500000
 *
 * `state` is one of ONLINE, REAUTHENTICATING, AUTH_REQUIRED, OFFLINE, UNKNOWN.
 * `updated` is the Unix time the script last refreshed the file. ODAC refuses a
 * stale or malformed file and then treats the link as unproven, which keeps
 * forwarding closed. The script process is reported as a diagnostic only and
 * never counts as proof of authentication. */
static struct {
    time_t checked, success, failure;
    long long proof_ms;
    char state[20];
    const char *error;
    int forwarding;
} wan = {.state = "UNKNOWN"};

static long long now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return -1;
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
int odac_platform_wan_proven(struct odac *ctx)
{
    (void)ctx;
    return odac_platform_wan_remaining() > 0;
}
long long odac_platform_wan_remaining(void)
{
    long long t = now_ms();
    if (strcmp(wan.state, "ONLINE") || wan.proof_ms <= 0 || t < wan.proof_ms) return 0;
    long long left = (ODAC_PLATFORM_LEASE * 1000LL - (t - wan.proof_ms)) / 1000;
    return left > 0 ? left : 0;
}
int odac_platform_status_file(const char *path, time_t now, int max_age, char state[20])
{
    state[0] = 0;
    if (!path || path[0] != '/' || strstr(path, "/../")) return -1;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return -1;
    struct stat st;
    char text[257];
    ssize_t n = -1;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_uid == ODAC_STATUS_OWNER && !(st.st_mode & 022) && st.st_size < 256)
        n = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (n <= 0) return -1;
    text[n] = 0;
    /* Exact format, nothing else: "state=STATE\nupdated=EPOCH\n" */
    if (strlen(text) != (size_t)n || strncmp(text, "state=", 6)) return -1;
    char *nl = strchr(text + 6, '\n');
    size_t len = nl ? (size_t)(nl - text - 6) : 0;
    if (!len || len >= 20 || strncmp(nl + 1, "updated=", 8)) return -1;
    char *digits = nl + 9, *end = digits;
    long long updated = 0;
    while (*end >= '0' && *end <= '9' && end - digits < 12) updated = updated * 10 + (*end++ - '0');
    if (end == digits || end[0] != '\n' || end[1] || (digits[0] == '0' && end - digits > 1)) return -1;
    char s[20];
    memcpy(s, text + 6, len); s[len] = 0;
    if (updated > (long long)now + 5 || (long long)now - updated > max_age) return -2;
    const char *allowed[] = {"ONLINE", "REAUTHENTICATING", "AUTH_REQUIRED", "OFFLINE", "UNKNOWN"};
    for (size_t i = 0; i < 5; i++) if (!strcmp(s, allowed[i])) { strcpy(state, s); return 0; }
    return -1;
}
static int script_running(struct odac *ctx)
{
    const char *path = ctx->config.pid_file;
    if (!path[0]) return -1;
    char buf[32];
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return 0;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    char *end; errno = 0;
    long pid = strtol(buf, &end, 10);
    while (*end == '\n' || *end == ' ') end++;
    if (errno || *end || pid < 2 || pid > 4194304) return 0;
    char proc[64]; snprintf(proc, sizeof(proc), "/proc/%ld", pid);
    return !access(proc, F_OK);
}
static void add_time(struct json_object *o, const char *key, time_t t)
{
    json_object_object_add(o, key, t ? json_object_new_int64((int64_t)t) : NULL);
}
struct json_object *odac_platform_wan(struct odac *ctx, time_t now)
{
    if (!ctx) return NULL;
    int interval = ctx->config.wan_interval < 5 ? 5 : (ctx->config.wan_interval > 60 ? 60 : ctx->config.wan_interval);
    if (!wan.checked || now < wan.checked || now - wan.checked >= interval) {
        const char *state = "UNKNOWN", *error = NULL;
        char file_state[20] = "";
        int max_age = interval * 3 > 300 ? 300 : (interval * 3 < 30 ? 30 : interval * 3);
        int file_rc = odac_platform_status_file(ctx->config.status_file, now, max_age, file_state);
        if (file_rc == 0) {
            state = file_state;
            error = strcmp(file_state, "ONLINE") ? "STATUS_FILE_REPORTED" : NULL;
        } else {
            error = file_rc == -2 ? "STATUS_FILE_STALE" : file_rc == -3 ? "STATUS_FILE_MISSING" : "STATUS_FILE_INVALID";
        }
        snprintf(wan.state, sizeof(wan.state), "%s", state);
        wan.error = error; wan.checked = now;
        if (!strcmp(state, "ONLINE")) { wan.success = now; wan.proof_ms = now_ms(); }
        else { wan.failure = now; wan.proof_ms = 0; }
        wan.forwarding = odac_platform_gate(ctx, !strcmp(state, "ONLINE")) == 0 && !strcmp(state, "ONLINE");
    }
    struct json_object *o = json_object_new_object();
    json_object_object_add(o, "state", json_object_new_string(wan.state));
    json_object_object_add(o, "upstream", json_object_new_string(ctx->config.upstream));
    add_time(o, "checked_at", wan.checked);
    add_time(o, "last_success", wan.success);
    add_time(o, "last_failure", wan.failure);
    int running = script_running(ctx);
    json_object_object_add(o, "script_running", running < 0 ? NULL : json_object_new_boolean(running));
    json_object_object_add(o, "error", wan.error ? json_object_new_string(wan.error) : NULL);
    json_object_object_add(o, "forwarding", json_object_new_boolean(wan.forwarding && odac_platform_wan_proven(ctx)));
    return o;
}
