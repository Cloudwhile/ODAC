/* SPDX-License-Identifier: GPL-2.0-only
 * Host tests for ODAC platform code. Run: sh tests/run_platform_tests.sh
 * (builds with compile-time fixture paths and runs inside unshare -rnm).
 * Fixture doubles live only under tests/ and are selected at compile time.
 */
#define _GNU_SOURCE
#include "platform.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static const char *nds_all =
    "{\"client_list_length\":\"2\",\"clients\":{"
    "\"aa:bb:cc:dd:ee:01\":{\"mac\":\"aa:bb:cc:dd:ee:01\",\"ip\":\"192.168.50.10\",\"token\":\"0123abcd\",\"state\":\"Preauthenticated\",\"upload_this_session\":\"2\",\"download_this_session\":\"3\",\"last_active\":\"100\"},"
    "\"aa:bb:cc:dd:ee:02\":{\"mac\":\"aa:bb:cc:dd:ee:02\",\"ip\":\"192.168.50.11\",\"token\":\"89abcdef\",\"state\":\"Authenticated\"}}}";

static void validators(void)
{
    CHECK(odac_platform_valid_interface("br-guest"));
    CHECK(odac_platform_valid_interface("wlan0.1"));
    const char *bad_if[] = {"", "lo", "-eth0", "br guest", "eth0;nft", "br-guest-name-too", "eth0\n", "*", NULL};
    for (int i = 0; bad_if[i]; i++) CHECK(!odac_platform_valid_interface(bad_if[i]));
    CHECK(odac_platform_valid_mac("aa:bb:cc:dd:ee:01"));
    const char *bad_mac[] = {"01:00:5e:00:00:01", "ff:ff:ff:ff:ff:ff", "00:00:00:00:00:00", "aa-bb-cc-dd-ee-01", "aa:bb:cc:dd:ee:1", "aa:bb:cc:dd:ee:011", "aa:bb:cc:dd:ee:0g", NULL};
    for (int i = 0; bad_mac[i]; i++) CHECK(!odac_platform_valid_mac(bad_mac[i]));
    CHECK(odac_platform_valid_ipv4("192.168.50.10"));
    const char *bad_ip[] = {"0.0.0.0", "127.0.0.1", "224.0.0.1", "255.255.255.255", "192.168.50", "192.168.50.010x", " 192.168.50.10", "::1", NULL};
    for (int i = 0; bad_ip[i]; i++) CHECK(!odac_platform_valid_ipv4(bad_ip[i]));
}

static void ndsctl_parser(void)
{
    struct json_object *root = odac_platform_parse_json(nds_all), *c;
    CHECK(root != NULL);
    c = odac_platform_parse_client(root, "192.168.50.10", NULL);
    CHECK(c && !strcmp(odac_platform_string(c, "mac"), "aa:bb:cc:dd:ee:01"));
    if (c) json_object_put(c);
    c = odac_platform_parse_client(root, NULL, "AA:BB:CC:DD:EE:02");
    CHECK(c && !strcmp(odac_platform_string(c, "state"), "Authenticated"));
    if (c) json_object_put(c);
    CHECK(!odac_platform_parse_client(root, "192.168.50.99", NULL));
    CHECK(!odac_platform_parse_client(root, "192.168.50.10", "aa:bb:cc:dd:ee:02"));
    CHECK(!odac_platform_parse_client(root, NULL, NULL));
    json_object_put(root);

    const char *bad[] = {
        "", "[]", "{", "{} trailing", "{\"clients\":[]}",
        /* key/mac mismatch cannot be used to smuggle another identity */
        "{\"clients\":{\"aa:bb:cc:dd:ee:09\":{\"mac\":\"aa:bb:cc:dd:ee:01\",\"ip\":\"192.168.50.10\",\"state\":\"Authenticated\"}}}",
        /* duplicate IP across two clients is ambiguous */
        "{\"clients\":{\"aa:bb:cc:dd:ee:01\":{\"mac\":\"aa:bb:cc:dd:ee:01\",\"ip\":\"192.168.50.10\",\"state\":\"Authenticated\"},\"aa:bb:cc:dd:ee:02\":{\"mac\":\"aa:bb:cc:dd:ee:02\",\"ip\":\"192.168.50.10\",\"state\":\"Authenticated\"}}}",
        "{\"mac\":\"aa:bb:cc:dd:ee:01\",\"ip\":\"192.168.50.10\",\"state\":\"Trusted\"}",
        "{\"mac\":\"aa:bb:cc:dd:ee:01\",\"ip\":192,\"state\":\"Authenticated\"}",
        "{\"mac\":\"aa:bb:cc:dd:ee:01\",\"ip\":\"192.168.50.10\"}",
        "{\"mac\":\"aa:bb:cc:dd:ee:01\",\"ip\":\"192.168.50.10\\u0000x\",\"state\":\"Authenticated\"}",
        NULL};
    for (int i = 0; bad[i]; i++) {
        struct json_object *r = odac_platform_parse_json(bad[i]);
        struct json_object *x = r ? odac_platform_parse_client(r, "192.168.50.10", NULL) : NULL;
        if (x) fprintf(stderr, "accepted bad fixture %d\n", i);
        CHECK(!x);
        if (x) json_object_put(x);
        if (r) json_object_put(r);
    }
    /* Single-client form: "ndsctl json IP" */
    root = odac_platform_parse_json("{\"mac\":\"aa:bb:cc:dd:ee:01\",\"ip\":\"192.168.50.10\",\"state\":\"Preauthenticated\"}\n");
    c = root ? odac_platform_parse_client(root, "192.168.50.10", NULL) : NULL;
    CHECK(c != NULL);
    if (c) json_object_put(c);
    if (root) json_object_put(root);
    root = odac_platform_parse_json("{}\n");
    CHECK(root && !odac_platform_parse_client(root, "192.168.50.10", NULL));
    if (root) json_object_put(root);
}

static void neighbors(void)
{
    const char *ok = "  \n192.168.50.10 dev br-guest lladdr aa:bb:cc:dd:ee:01 REACHABLE\n"
                     "192.168.1.5 dev br-lan lladdr aa:bb:cc:dd:ee:05 STALE\n";
    CHECK(odac_platform_neighbor_matches(ok, "br-guest", "192.168.50.10", "AA:BB:CC:DD:EE:01"));
    CHECK(!odac_platform_neighbor_matches(ok, "br-guest", "192.168.1.5", "aa:bb:cc:dd:ee:05"));
    CHECK(!odac_platform_neighbor_matches(ok, "br-gues", "192.168.50.10", "aa:bb:cc:dd:ee:01"));
    CHECK(!odac_platform_neighbor_matches(ok, "br-guest", "192.168.50.10", "aa:bb:cc:dd:ee:02"));
    CHECK(!odac_platform_neighbor_matches("192.168.50.10 dev br-guest lladdr aa:bb:cc:dd:ee:01 FAILED\n", "br-guest", "192.168.50.10", "aa:bb:cc:dd:ee:01"));
    CHECK(!odac_platform_neighbor_matches("192.168.50.10 dev br-guest  INCOMPLETE\n", "br-guest", "192.168.50.10", "aa:bb:cc:dd:ee:01"));
    CHECK(!odac_platform_neighbor_matches("192.168.50.10 dev br-guest lladdr aa:bb:cc:dd:ee:01 REACHABLE\n"
                                          "192.168.50.10 dev br-lan lladdr aa:bb:cc:dd:ee:01 REACHABLE\n", "br-guest", "192.168.50.10", "aa:bb:cc:dd:ee:01"));
    CHECK(!odac_platform_neighbor_matches("192.168.50.10 dev br-guest lladdr aa:bb:cc:dd:ee:01 router REACHABLE\n", "br-guest", "192.168.50.10", "aa:bb:cc:dd:ee:01"));
    CHECK(!odac_platform_neighbor_matches("", "br-guest", "192.168.50.10", "aa:bb:cc:dd:ee:01"));
}

static void runner(void)
{
    char out[16];
    const char *echo[] = {"/bin/echo", "ok;$(id)", NULL};
    CHECK(odac_platform_run(echo, out, sizeof(out), 1000) == 0 && !strcmp(out, "ok;$(id)\n"));
    const char *relative[] = {"echo", "x", NULL};
    CHECK(odac_platform_run(relative, out, sizeof(out), 1000) == -1);
    const char *missing[] = {"/nonexistent/odac", NULL};
    CHECK(odac_platform_run(missing, out, sizeof(out), 1000) == -1);
    const char *exit3[] = {"/bin/sh", "-c", "exit 3", NULL};
    CHECK(odac_platform_run(exit3, out, sizeof(out), 1000) == 3);
    const char *slow[] = {"/bin/sleep", "5", NULL};
    CHECK(odac_platform_run(slow, out, sizeof(out), 100) == -1);
    const char *big[] = {"/bin/sh", "-c", "yes | head -c 100000", NULL};
    CHECK(odac_platform_run(big, out, sizeof(out), 1000) == -1);
    /* Grandchild holding the pipe is killed with the process group. */
    const char *daemon[] = {"/bin/sh", "-c", "sleep 5 & exit 0", NULL};
    CHECK(odac_platform_run(daemon, out, sizeof(out), 200) == -1);
}

static void write_file(const char *path, const char *text, mode_t mode)
{
    unlink(path);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    CHECK(fd >= 0);
    if (fd < 0) return;
    CHECK(write(fd, text, strlen(text)) == (ssize_t)strlen(text));
    fchmod(fd, mode);
    close(fd);
}

static void status_file(void)
{
    char path[] = "/tmp/odac-status-XXXXXX", state[20], text[128];
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    close(fd);
    time_t now = 1000000;
    snprintf(text, sizeof(text), "state=ONLINE\nupdated=%lld\n", (long long)now - 10);
    write_file(path, text, 0600);
    CHECK(odac_platform_status_file(path, now, 60, state) == 0 && !strcmp(state, "ONLINE"));
    snprintf(text, sizeof(text), "state=AUTH_REQUIRED\nupdated=%lld\n", (long long)now);
    write_file(path, text, 0644);
    CHECK(odac_platform_status_file(path, now, 60, state) == 0 && !strcmp(state, "AUTH_REQUIRED"));
    CHECK(odac_platform_status_file(path, now + 61, 60, state) == -2);
    CHECK(odac_platform_status_file(path, now - 6, 60, state) == -2);
    write_file(path, text, 0666);
    CHECK(odac_platform_status_file(path, now, 60, state) == -1);
    const char *bad[] = {"state=ONLINE\n", "state=online\nupdated=1000000\n", "state=GOOD\nupdated=1000000\n",
        "state=ONLINE\nupdated=1000000\nextra=1\n", "state=ONLINE\nupdated=1000000", "state=ONLINE\nupdated=-1\n",
        "state=ONLINE\nupdated=01000000\n", "state=ONLINE \nupdated=1000000\n", "password=x\nstate=ONLINE\nupdated=1000000\n", NULL};
    for (int i = 0; bad[i]; i++) {
        write_file(path, bad[i], 0600);
        CHECK(odac_platform_status_file(path, now, 60, state) == -1);
    }
    char link[] = "/tmp/odac-status-link-XXXXXX";
    fd = mkstemp(link); close(fd); unlink(link);
    CHECK(symlink(path, link) == 0);
    CHECK(odac_platform_status_file(link, now, 60, state) == -1);
    unlink(link); unlink(path);
    CHECK(odac_platform_status_file("relative", now, 60, state) == -1);
    CHECK(odac_platform_status_file("/tmp/does-not-exist-odac", now, 60, state) == -1);
}

static void wan_rules(void)
{
    const char *allowed[] = {"ONLINE", "REAUTHENTICATING", "AUTH_REQUIRED", "OFFLINE", "UNKNOWN", NULL};
    char path[] = "/tmp/odac-wan-XXXXXX", state[20], text[128];
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    close(fd);
    time_t now = 1000000;
    for (int i = 0; allowed[i]; i++) {
        snprintf(text, sizeof(text), "state=%s\nupdated=%lld\n", allowed[i], (long long)now);
        write_file(path, text, 0600);
        CHECK(odac_platform_status_file(path, now, 60, state) == 0 && !strcmp(state, allowed[i]));
    }
    snprintf(text, sizeof(text), "state=ONLINE\nupdated=%lld\n", (long long)now - 120);
    write_file(path, text, 0600);
    CHECK(odac_platform_status_file(path, now, 60, state) == -2);
    unlink(path);
}

static void config(struct odac *ctx, const char *iface)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->config.enabled = 1;
    snprintf(ctx->config.interface, sizeof(ctx->config.interface), "%s", iface);
    snprintf(ctx->config.gateway, sizeof(ctx->config.gateway), "192.168.50.1");
    snprintf(ctx->config.upstream, sizeof(ctx->config.upstream), "203.0.113.10 203.0.113.20");
    ctx->config.portal_port = 2080;
    ctx->config.portal_https_port = 8443;
    ctx->config.max_devices = 16;
    ctx->config.wan_interval = 30;
}

static char *slurp(const char *path)
{
    static char buf[8192];
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0; fclose(f);
    return buf;
}

/* Uses fixture doubles that log argv to $ODAC_TEST_LOG. */
static void platform_commands(const char *log)
{
    struct odac ctx;
    config(&ctx, "lo");
    CHECK(odac_platform_init(&ctx) == -1);           /* loopback never controlled */
    config(&ctx, "nonexistent9");
    CHECK(odac_platform_init(&ctx) == -1);           /* interface must exist */
    config(&ctx, "odactest0");
    CHECK(truncate(log, 0) == 0);
    if (odac_platform_init(&ctx)) { fprintf(stderr, "skip: odactest0 dummy interface absent\n"); return; }
    /* Configured upstream portal addresses reach the guard as separate arguments. */
    char *init_log = slurp(log);
    CHECK(init_log && strstr(init_log, "fw|init|odactest0|192.168.50.1|2080|8443|203.0.113.10|203.0.113.20|\n"));
    struct json_object *c = odac_platform_client(&ctx, "192.168.50.10");
    CHECK(c && !strcmp(odac_platform_string(c, "mac"), "aa:bb:cc:dd:ee:01"));
    if (c) json_object_put(c);
    CHECK(!odac_platform_client(&ctx, "192.168.50.11")); /* in ndsctl, absent in neighbor table */
    CHECK(!odac_platform_client(&ctx, "192.168.50.12")); /* neighbor on another interface */
    CHECK(!odac_platform_client(&ctx, "192.168.50.99"));
    CHECK(!odac_platform_client(&ctx, "192.168.50.10;reboot"));
    struct json_object *devices = odac_platform_devices(&ctx), *v;
    CHECK(devices && json_object_array_length(devices) == 1);
    struct json_object *d = devices ? json_object_array_get_idx(devices, 0) : NULL;
    CHECK(d && json_object_object_get_ex(d, "ipv6", &v) && v == NULL);
    CHECK(d && json_object_object_get_ex(d, "upload_bytes", &v) && json_object_get_int64(v) == 2048);
    CHECK(d && !strcmp(odac_platform_string(d, "auth_state"), "WAITING"));
    if (devices) json_object_put(devices);

    CHECK(truncate(log, 0) == 0);
    CHECK(odac_platform_auth(&ctx, "aa:bb:cc:dd:ee:01", 3600, 10000, 30000) == 0);
    char *text = slurp(log);
    CHECK(text && strstr(text, "ndsctl|auth|0123abcd|60|10000|30000|0|0|\n"));
    CHECK(text && strstr(text, "fw|auth|odactest0|aa:bb:cc:dd:ee:01|192.168.50.10|90|\n"));
    CHECK(text && !strstr(text, "trust"));
    CHECK(truncate(log, 0) == 0);
    CHECK(odac_platform_auth(&ctx, "aa:bb:cc:dd:ee:01", 30, 10000, 30000) == 0); /* refresh */
    text = slurp(log);
    CHECK(text && strstr(text, "fw|auth|odactest0|aa:bb:cc:dd:ee:01|192.168.50.10|30|\n"));
    CHECK(odac_platform_auth(&ctx, "aa:bb:cc:dd:ee:02", 60, 0, 0) == -1); /* no verified neighbor */
    CHECK(odac_platform_auth(&ctx, "aa:bb:cc:dd:ee:01", 0, 0, 0) == -1);
    CHECK(odac_platform_auth(&ctx, "aa:bb:cc:dd:ee:01", 60, -1, 0) == -1);
    CHECK(odac_platform_auth(&ctx, "ff:ff:ff:ff:ff:ff", 60, 0, 0) == -1);
    CHECK(text && !strstr(text, "ndsctl|auth|")); /* lease refresh keeps counters */
    CHECK(truncate(log, 0) == 0);
    CHECK(odac_platform_auth(&ctx, "aa:bb:cc:dd:ee:01", 61, 0, 15000) == 0);
    text = slurp(log);
    CHECK(text && strstr(text, "ndsctl|deauth|aa:bb:cc:dd:ee:01|\n"));
    CHECK(text && strstr(text, "ndsctl|auth|aa:bb:cc:dd:ee:01|2|0|15000|0|0|\n"));

    CHECK(truncate(log, 0) == 0);
    CHECK(odac_platform_deauth(&ctx, "aa:bb:cc:dd:ee:01") == 0);
    text = slurp(log);
    CHECK(text && strstr(text, "fw|deauth|odactest0|aa:bb:cc:dd:ee:01|192.168.50.10|\n"));
    CHECK(text && strstr(text, "ndsctl|deauth|aa:bb:cc:dd:ee:01|\n"));

    CHECK(truncate(log, 0) == 0);
    CHECK(odac_platform_gate(&ctx, 1) == -1); /* no WAN proof: never opens */
    text = slurp(log);
    CHECK(text && strstr(text, "fw|gate|odactest0|closed|90|\n") && !strstr(text, "online"));

    /* openNDS deleted the client on deauth: counters are unknown, never invented. */
    devices = odac_platform_devices(&ctx);
    d = devices ? json_object_array_get_idx(devices, 0) : NULL;
    CHECK(d && json_object_object_get_ex(d, "upload_bytes", &v) && v == NULL);
    CHECK(d && json_object_object_get_ex(d, "auth_state", &v) && v == NULL);
    if (devices) json_object_put(devices);

    /* Neighbor-only IoT discovery remains visible; portal identity is absent,
     * while an explicit core policy may preemptively authorize this binding. */
    CHECK(!odac_platform_client(&ctx, "192.168.50.10"));
    CHECK(truncate(log, 0) == 0);
    CHECK(odac_platform_auth(&ctx, "aa:bb:cc:dd:ee:01", 120, 4000, 8000) == 0);
    text = slurp(log);
    CHECK(text && strstr(text, "ndsctl|auth|aa:bb:cc:dd:ee:01|2|4000|8000|0|0|\n"));
    CHECK(odac_platform_deauth(&ctx, "aa:bb:cc:dd:ee:01") == 0);

    /* Without an administrator status file the link stays unproven. */
    struct json_object *w = odac_platform_wan(&ctx, 1000);
    CHECK(w && !strcmp(odac_platform_string(w, "state"), "UNKNOWN"));
    CHECK(w && !strcmp(odac_platform_string(w, "error"), "STATUS_FILE_INVALID"));
    CHECK(w && json_object_object_get_ex(w, "forwarding", &v) && !json_object_get_boolean(v));
    if (w) json_object_put(w);
    odac_platform_close(&ctx);
    CHECK(odac_platform_auth(&ctx, "aa:bb:cc:dd:ee:01", 60, 0, 0) == -1); /* closed */
}

int main(void)
{
    validators();
    ndsctl_parser();
    neighbors();
    runner();
    status_file();
    wan_rules();
    const char *log = getenv("ODAC_TEST_LOG");
    if (log) platform_commands(log);
    if (failures) { fprintf(stderr, "%d platform check(s) failed\n", failures); return 1; }
    puts("platform_test: ok");
    return 0;
}
