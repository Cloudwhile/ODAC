/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "platform.h"
#include <ctype.h>
#include <errno.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#ifndef ODAC_NDSCTL
#define ODAC_NDSCTL "/usr/bin/ndsctl"
#endif
#ifndef ODAC_IP
#define ODAC_IP "/sbin/ip"
#endif
#ifndef ODAC_FIREWALL
#define ODAC_FIREWALL "/usr/libexec/odac-firewall"
#endif
#ifndef ODAC_CONNTRACK
#define ODAC_CONNTRACK "/usr/sbin/conntrack"
#endif
#ifndef ODAC_UBUS
#define ODAC_UBUS "/bin/ubus"
#endif
#ifndef ODAC_DHCP_LEASES
#define ODAC_DHCP_LEASES "/tmp/dhcp.leases"
#endif
#define CLIENT_LIMIT 1024
/* The daemon is a single-instance serialized event loop. Core owns ctx->runtime. */
static struct odac *owner;
static struct grant { char mac[18], ip[16]; int upload, download; } grants[CLIENT_LIMIT];
static int ready;
static int command(const char *const argv[])
{
    char out[4096];
    int rc = odac_platform_run(argv, out, sizeof(out), 4000);
    /* ndsctl exits 4 while its global lock is busy: one bounded retry. */
    if (rc == 4 && !strcmp(argv[0], ODAC_NDSCTL)) rc = odac_platform_run(argv, out, sizeof(out), 4000);
    return rc;
}
/* ndsctl deauth exits 1 ("No") for an absent/unauthenticated client. */
static int nds_deauth(const char *selector)
{
    const char *argv[] = {ODAC_NDSCTL, "deauth", selector, NULL};
    int rc = command(argv);
    return rc == 0 || rc == 1 ? 0 : -1;
}
static struct json_object *nds(const char *selector)
{
    char *out = malloc(ODAC_PLATFORM_OUTPUT);
    if (!out) return NULL;
    const char *argv[] = {ODAC_NDSCTL, "json", selector, NULL};
    struct json_object *j = NULL;
    int rc = odac_platform_run(argv, out, ODAC_PLATFORM_OUTPUT, 4000);
    if (rc == 4) rc = odac_platform_run(argv, out, ODAC_PLATFORM_OUTPUT, 4000);
    if (!rc) j = odac_platform_parse_json(out);
    free(out); return j;
}
static char *neighbors(void)
{
    char *out = malloc(ODAC_PLATFORM_OUTPUT);
    if (!out) return NULL;
    /* "show dev X" omits the dev column; read all and match the device exactly. */
    const char *argv[] = {ODAC_IP, "-4", "neigh", "show", NULL};
    if (odac_platform_run(argv, out, ODAC_PLATFORM_OUTPUT, 3000)) { free(out); return NULL; }
    return out;
}
static struct json_object *verified(struct odac *ctx, const char *ip, const char *mac)
{
    if (ctx != owner || !ready) return NULL;
    struct json_object *root = nds(ip ? ip : mac);
    if (!root) return NULL;
    struct json_object *client = odac_platform_parse_client(root, ip, mac);
    json_object_put(root);
    if (!client) return NULL;
    char *neigh = neighbors();
    int ok = neigh && odac_platform_neighbor_matches(neigh, ctx->config.interface,
        odac_platform_string(client, "ip"), odac_platform_string(client, "mac"));
    free(neigh);
    if (!ok) { json_object_put(client); return NULL; }
    return client;
}
int odac_platform_init(struct odac *ctx)
{
    if (!ctx || !ctx->config.enabled || !odac_platform_valid_interface(ctx->config.interface) ||
        !odac_platform_valid_ipv4(ctx->config.gateway) || !if_nametoindex(ctx->config.interface) ||
        ctx->config.portal_port < 1 || ctx->config.portal_port > 65535 ||
        ctx->config.portal_https_port < 1 || ctx->config.portal_https_port > 65535) return -1;
    char port[8], tls[8], upstream[sizeof(ctx->config.upstream)], *save = NULL;
    snprintf(port, sizeof(port), "%d", ctx->config.portal_port);
    snprintf(tls, sizeof(tls), "%d", ctx->config.portal_https_port);
    /* Upstream portal addresses come only from configuration (validated at load). */
    const char *argv[6 + 16 + 1] = {ODAC_FIREWALL, "init", ctx->config.interface, ctx->config.gateway, port, tls};
    size_t argc = 6;
    snprintf(upstream, sizeof(upstream), "%s", ctx->config.upstream);
    for (char *token = strtok_r(upstream, " ", &save); token; token = strtok_r(NULL, " ", &save)) {
        if (argc >= 6 + 16) return -1;
        argv[argc++] = token;
    }
    argv[argc] = NULL;
    if (command(argv)) return -1;
    memset(grants, 0, sizeof(grants)); owner = ctx; ready = 1;
    return 0;
}
void odac_platform_close(struct odac *ctx)
{
    if (ctx != owner || !ready) return;
    const char *argv[] = {ODAC_FIREWALL, "close", ctx->config.interface, NULL};
    /* Even if nft is unavailable, the previously issued leases expire. */
    if (command(argv)) fprintf(stderr, "odac: guard close failed; leases will expire\n");
    memset(grants, 0, sizeof(grants)); ready = 0; owner = NULL;
}
struct json_object *odac_platform_client(struct odac *ctx, const char *ip)
{
    if (!odac_platform_valid_ipv4(ip)) return NULL;
    struct json_object *raw = verified(ctx, ip, NULL);
    if (!raw) return NULL;
    struct json_object *o = json_object_new_object();
    const char *keys[] = {"mac", "ip", "state"};
    for (size_t i = 0; i < 3; i++) json_object_object_add(o, keys[i], json_object_new_string(odac_platform_string(raw, keys[i])));
    json_object_put(raw); return o;
}
static int lease_name(char *out, size_t capacity, const char *mac, const char *ip)
{
    FILE *leases = fopen(ODAC_DHCP_LEASES, "r");
    if (!leases) return 0;
    char row[1024]; int count = 0, found = 0;
    while (!found && count++ < 4096 && fgets(row, sizeof(row), leases)) {
        unsigned long long expiry; char lm[32], li[64], host[256];
        if (sscanf(row, "%llu %31s %63s %255s", &expiry, lm, li, host) == 4 && !strcasecmp(lm, mac) &&
            !strcmp(li, ip) && strcmp(host, "*") && (expiry == 0 || expiry > (unsigned long long)time(NULL)) &&
            strlen(host) < capacity) {
            snprintf(out, capacity, "%s", host); found = 1;
        }
    }
    fclose(leases);
    return found;
}
static int hint_name(char *out, size_t capacity, const char *mac)
{
    char *text = malloc(ODAC_PLATFORM_OUTPUT);
    if (!text) return 0;
    struct json_object *hints = NULL;
    const char *ubus[] = {ODAC_UBUS, "call", "luci-rpc", "getHostHints", "{}", NULL};
    if (!odac_platform_run(ubus, text, ODAC_PLATFORM_OUTPUT, 2000)) hints = odac_platform_parse_json(text);
    free(text);
    int found = 0;
    if (hints) {
        json_object_object_foreach(hints, key, hint) {
            const char *name = odac_platform_string(hint, "name");
            if (!strcasecmp(key, mac) && name && *name && strlen(name) < capacity) {
                snprintf(out, capacity, "%s", name); found = 1; break;
            }
        }
        json_object_put(hints);
    }
    return found;
}
/* Names help administrators identify a registration; they never establish identity. */
int odac_platform_hostname(struct odac *ctx, const char *mac, const char *ip, char *out, size_t capacity)
{
    if (!out || capacity < 2) return -1;
    out[0] = '\0';
    if (ctx != owner || !ready || !odac_platform_valid_mac(mac) || !odac_platform_valid_ipv4(ip)) return -1;
    return lease_name(out, capacity, mac, ip) || hint_name(out, capacity, mac) ? 0 : -1;
}
static struct grant *grant_slot(const char *mac, int create)
{
    struct grant *empty = NULL;
    for (size_t i = 0; i < CLIENT_LIMIT; i++) {
        if (grants[i].mac[0] && !strcasecmp(grants[i].mac, mac)) return &grants[i];
        if (!grants[i].mac[0] && !empty) empty = &grants[i];
    }
    return create ? empty : NULL;
}
static int revoke_guard(struct odac *ctx, const char *mac, const char *ip)
{
    const char *argv[] = {ODAC_FIREWALL, "deauth", ctx->config.interface, mac, ip, NULL};
    if (!command(argv)) return 0;
    /* A selective revoke failure closes the entire gate, never keeps a grant. */
    odac_platform_gate(ctx, 0);
    return -1;
}
int odac_platform_auth(struct odac *ctx, const char *mac, int seconds, int upload, int download)
{
    if (!odac_platform_valid_mac(mac) || seconds < 1 || seconds > 31536000 || upload < 0 || upload > 10000000 || download < 0 || download > 10000000) return -1;
    if (ctx != owner || !ready) return -1;
    /* Identity source: one exact neighbor binding on the controlled interface.
     * An openNDS record, if present, must agree; otherwise auth fails. */
    char ip[16], *neigh = neighbors();
    int bound = neigh && !odac_platform_neighbor_ip(neigh, ctx->config.interface, mac, ip);
    free(neigh);
    if (!bound) return -1;
    struct json_object *root = nds(mac), *raw = NULL;
    if (!root) return -1;
    if (json_object_object_length(root)) {
        raw = odac_platform_parse_client(root, ip, mac);
        if (!raw) { json_object_put(root); return -1; }
    }
    json_object_put(root);
    const char *state = raw ? odac_platform_string(raw, "state") : "";
    struct grant *g = grant_slot(mac, 1);
    if (!g) goto fail;
    if (g->mac[0] && strcmp(g->ip, ip) && revoke_guard(ctx, g->mac, g->ip)) goto fail;
    int unchanged = g->mac[0] && !strcmp(g->ip, ip) && g->upload == upload && g->download == download && !strcmp(state, "Authenticated");
    if (!unchanged) {
        if (revoke_guard(ctx, mac, ip)) goto fail;
        const char *selector = mac;
        if (!strcmp(state, "Authenticated")) {
            /* openNDS fixes rates at auth and deletes an authenticated client on
             * deauth; the MAC re-auth then requires allow_preemptive_authentication. */
            if (nds_deauth(mac)) goto fail;
        } else if (!strcmp(state, "Preauthenticated")) {
            /* The token binds this exact observed openNDS client incarnation. */
            const char *token = odac_platform_string(raw, "token");
            int token_ok = token && strlen(token) >= 8 && strlen(token) <= 128;
            if (token_ok) for (const char *p = token; *p; p++) if (!isxdigit((unsigned char)*p)) token_ok = 0;
            if (token_ok) selector = token;
        }
        char minutes[16], up[16], down[16];
        snprintf(minutes, sizeof(minutes), "%d", (seconds + 59) / 60);
        snprintf(up, sizeof(up), "%d", upload); snprintf(down, sizeof(down), "%d", download);
        const char *argv[] = {ODAC_NDSCTL, "auth", selector, minutes, up, down, "0", "0", NULL};
        if (command(argv)) { nds_deauth(mac); goto fail; }
        struct json_object *after = verified(ctx, ip, mac);
        int ok = after && !strcmp(odac_platform_string(after, "state"), "Authenticated");
        if (after) json_object_put(after);
        if (!ok) { nds_deauth(mac); goto fail; }
    }
    char lease[8]; snprintf(lease, sizeof(lease), "%d", seconds < ODAC_PLATFORM_LEASE ? seconds : ODAC_PLATFORM_LEASE);
    const char *fw[] = {ODAC_FIREWALL, "auth", ctx->config.interface, mac, ip, lease, NULL};
    if (command(fw)) { revoke_guard(ctx, mac, ip); nds_deauth(mac); goto fail; }
    snprintf(g->mac, sizeof(g->mac), "%s", mac); snprintf(g->ip, sizeof(g->ip), "%s", ip);
    g->upload = upload; g->download = download;
    if (raw) json_object_put(raw);
    return 0;
fail:
    if (g && g->mac[0] && !strcasecmp(g->mac, mac)) memset(g, 0, sizeof(*g));
    if (raw) json_object_put(raw);
    return -1;
}
int odac_platform_deauth(struct odac *ctx, const char *mac)
{
    if (ctx != owner || !ready || !odac_platform_valid_mac(mac)) return -1;
    struct grant *g = grant_slot(mac, 0);
    struct json_object *raw = verified(ctx, NULL, mac);
    const char *ip = g ? g->ip : (raw ? odac_platform_string(raw, "ip") : NULL);
    int rc;
    if (ip) {
        rc = revoke_guard(ctx, mac, ip);
        const char *seen = raw ? odac_platform_string(raw, "ip") : NULL;
        if (seen && strcmp(seen, ip) && revoke_guard(ctx, mac, seen)) rc = -1;
    } else rc = 0; /* No grant since init recreated the table: nothing to revoke. */
    if (nds_deauth(mac)) rc = -1;
    if (ip) {
        const char *ct[] = {ODAC_CONNTRACK, "-D", "-s", ip, NULL};
        /* conntrack exits 1 for zero matches. Guard checks every packet anyway. */
        if (command(ct)) fprintf(stderr, "odac: conntrack cleanup unavailable or no matching flows\n");
    }
    if (g) memset(g, 0, sizeof(*g));
    if (raw) json_object_put(raw);
    return rc;
}
/* WAN module supplies a recent monotonic proof; no caller can open UNKNOWN. */
int odac_platform_gate(struct odac *ctx, int online)
{
    if (ctx != owner || !ready) return -1;
    int requested = online;
    /* The gate lease never outlives the WAN proof it is based on. */
    long long remaining = online ? odac_platform_wan_remaining() : 0;
    if (remaining < 1) online = 0;
    if (remaining > ODAC_PLATFORM_LEASE) remaining = ODAC_PLATFORM_LEASE;
    char lease[24];
    snprintf(lease, sizeof(lease), "%lld", online ? remaining : 90LL);
    const char *argv[] = {ODAC_FIREWALL, "gate", ctx->config.interface, online ? "online" : "closed", lease, NULL};
    int rc = command(argv);
    if (rc && online) {
        const char *close[] = {ODAC_FIREWALL, "close", ctx->config.interface, NULL};
        command(close);
    }
    return requested && !online ? -1 : rc;
}
static struct json_object *new_device(struct odac *ctx, const char *mac, const char *ip)
{
    struct json_object *d = json_object_new_object();
    const char *unknown[] = {"ipv6", "hostname", "alias", "access_type", "ssid", "first_seen", "last_seen", "username", "auth_state", "policy", "expires_at", "upload_kbps", "download_kbps", "upload_bytes", "download_bytes"};
    for (size_t i = 0; i < sizeof(unknown)/sizeof(*unknown); i++) json_object_object_add(d, unknown[i], NULL);
    json_object_object_add(d, "mac", json_object_new_string(mac));
    json_object_object_add(d, "ipv4", json_object_new_string(ip));
    json_object_object_add(d, "interface", json_object_new_string(ctx->config.interface));
    json_object_object_add(d, "online", NULL);
    return d;
}
static void numeric_field(struct json_object *d, const char *dest, struct json_object *src, const char *key, int multiplier)
{
    const char *s = odac_platform_string(src, key);
    if (!s || !*s || strlen(s) > 18) return;
    for (const char *p = s; *p; p++) if (!isdigit((unsigned char)*p)) return;
    errno = 0; unsigned long long n = strtoull(s, NULL, 10);
    if (errno || n > (unsigned long long)INT64_MAX / multiplier) return;
    json_object_object_add(d, dest, json_object_new_int64((int64_t)n * multiplier));
}
struct json_object *odac_platform_devices(struct odac *ctx)
{
    struct json_object *result = json_object_new_array();
    if (ctx != owner || !ready) return result;
    char *text = neighbors();
    if (!text) return result;
    struct json_object *all = nds(NULL);
    /* Optional rpcd host hints augment names; never establish identity. */
    char *hint_text = malloc(ODAC_PLATFORM_OUTPUT);
    struct json_object *hints = NULL;
    const char *ubus[] = {ODAC_UBUS, "call", "luci-rpc", "getHostHints", "{}", NULL};
    if (hint_text && !odac_platform_run(ubus, hint_text, ODAC_PLATFORM_OUTPUT, 2000)) hints = odac_platform_parse_json(hint_text);
    free(hint_text);
    char *copy = strdup(text), *save, *line;
    if (!copy) { free(text); if (all) json_object_put(all); if (hints) json_object_put(hints); return result; }
    for (line = strtok_r(copy, "\n", &save); line && json_object_array_length(result) < (size_t)ctx->config.max_devices; line = strtok_r(NULL, "\n", &save)) {
        char ip[64] = {0}, dev[32], mac[32], state[32];
        if (sscanf(line, "%63s dev %31s lladdr %31s %31s", ip, dev, mac, state) != 4 ||
            !odac_platform_neighbor_matches(text, ctx->config.interface, ip, mac)) continue;
        struct json_object *d = new_device(ctx, mac, ip);
        /* STALE proves a binding, not current presence. */
        if (!strcmp(state, "REACHABLE")) json_object_object_add(d, "online", json_object_new_boolean(1));
        struct json_object *client = all ? odac_platform_parse_client(all, ip, mac) : NULL;
        if (client) {
            const char *st = odac_platform_string(client, "state");
            json_object_object_add(d, "auth_state", json_object_new_string(!strcmp(st, "Authenticated") ? "AUTHENTICATED" : "WAITING"));
            numeric_field(d, "upload_bytes", client, "upload_this_session", 1024);
            numeric_field(d, "download_bytes", client, "download_this_session", 1024);
            numeric_field(d, "last_seen", client, "last_active", 1);
            json_object_put(client);
        }
        FILE *leases = fopen(ODAC_DHCP_LEASES, "r");
        if (leases) {
            char row[1024]; int count = 0;
            while (count++ < 4096 && fgets(row, sizeof(row), leases)) {
                unsigned long long expiry; char lm[32], li[64], host[256];
                if (sscanf(row, "%llu %31s %63s %255s", &expiry, lm, li, host) == 4 && !strcasecmp(lm, mac) && !strcmp(li, ip) && strcmp(host, "*") && (expiry == 0 || expiry > (unsigned long long)time(NULL))) {
                    json_object_object_add(d, "hostname", json_object_new_string(host)); break;
                }
            }
            fclose(leases);
        }
        if (hints) {
            json_object_object_foreach(hints, key, hint) {
                const char *name = odac_platform_string(hint, "name");
                if (!strcasecmp(key, mac) && name && *name && strlen(name) <= 255) {
                    json_object_object_add(d, "hostname", json_object_new_string(name)); break;
                }
            }
        }
        json_object_array_add(result, d);
    }
    free(copy); free(text);
    if (all) json_object_put(all);
    if (hints) json_object_put(hints);
    return result;
}
