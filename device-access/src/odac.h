#ifndef ODAC_H
#define ODAC_H
#include <json-c/json.h>
#include <sqlite3.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
struct odac_config {
 char database[256], socket_path[256], interface[32], gateway[64];
 char upstream[272], status_file[256], pid_file[256];
 int enabled, portal_port, session_seconds, max_users, max_devices, max_sessions;
 int upload_kbps, download_kbps, wan_interval, max_audit, db_max_kib;
 char portal_hostname[256], tls_cert[256], tls_key[256];
 int portal_https_port;
};
struct odac { sqlite3 *db; struct odac_config config; void *runtime; };
int odac_open(struct odac *ctx);
void odac_close(struct odac *ctx);
struct json_object *odac_dispatch(struct odac *ctx, struct json_object *request);
void odac_tick(struct odac *ctx, time_t now);
int odac_platform_init(struct odac *ctx);
void odac_platform_close(struct odac *ctx);
struct json_object *odac_platform_client(struct odac *ctx, const char *ip);
/* Best-effort device name from DHCP leases and rpcd host hints; recording only. */
int odac_platform_hostname(struct odac *ctx, const char *mac, const char *ip, char *out, size_t capacity);
int odac_platform_auth(struct odac *ctx, const char *mac, int seconds, int upload, int download);
int odac_platform_deauth(struct odac *ctx, const char *mac);
int odac_platform_gate(struct odac *ctx, int online);
struct json_object *odac_platform_wan(struct odac *ctx, time_t now);
struct json_object *odac_platform_devices(struct odac *ctx);
#endif
