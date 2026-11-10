#include "core.h"
#include "json_text.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>

struct json_object *core_parse(const char *text, size_t length, int depth)
{
 return odac_json_parse_strict(text, length, depth);
}

const char *core_str(struct json_object *o, const char *key)
{
 struct json_object *v = NULL;
 if (!o || !json_object_object_get_ex(o, key, &v) || !json_object_is_type(v, json_type_string)) return "";
 const char *s = json_object_get_string(v);
 return strlen(s) == (size_t)json_object_get_string_len(v) ? s : "";
}

int core_bool(struct json_object *o, const char *key)
{
 struct json_object *v = NULL;
 return o && json_object_object_get_ex(o, key, &v) && json_object_is_type(v, json_type_boolean) && json_object_get_boolean(v);
}

int core_has(struct json_object *o, const char *key)
{
 struct json_object *v = NULL;
 return o && json_object_object_get_ex(o, key, &v);
}

int core_number(struct json_object *o, const char *key, long long min, long long max, int nullable)
{
 struct json_object *v = NULL;
 if (!o || !json_object_object_get_ex(o, key, &v)) return 0;
 if (!v) return nullable;
 if (!json_object_is_type(v, json_type_int)) return 0;
 errno = 0;
 int64_t n = json_object_get_int64(v);
 return !errno && n >= min && n <= max;
}

long long core_ll(struct json_object *o, const char *key)
{
 struct json_object *v = NULL;
 if (!o || !json_object_object_get_ex(o, key, &v) || !json_object_is_type(v, json_type_int)) return 0;
 return (long long)json_object_get_int64(v);
}

int core_keys(struct json_object *o, const char *const *allowed)
{
 json_object_object_foreach(o, key, value) {
  int known = 0;
  (void)value;
  for (size_t i = 0; allowed[i]; i++) if (!strcmp(key, allowed[i])) { known = 1; break; }
  if (!known) return 0;
 }
 return 1;
}

void core_addstr(struct json_object *o, const char *key, const char *value)
{
 json_object_object_add(o, key, value ? json_object_new_string(value) : NULL);
}

void core_addint(struct json_object *o, const char *key, long long value, int present)
{
 json_object_object_add(o, key, present ? json_object_new_int64(value) : NULL);
}

int core_page(struct json_object *p, int *limit, int *offset)
{
 *limit = ODAC_PAGE_DEFAULT;
 *offset = 0;
 if (core_has(p, "limit")) { if (!core_number(p, "limit", 1, ODAC_PAGE_MAX, 0)) return -1; *limit = (int)core_ll(p, "limit"); }
 if (core_has(p, "offset")) { if (!core_number(p, "offset", 0, 1000000, 0)) return -1; *offset = (int)core_ll(p, "offset"); }
 return 0;
}

int core_bytes(void *buffer, size_t length)
{
 size_t done = 0;
 while (done < length) {
  ssize_t n = getrandom((char *)buffer + done, length - done, 0);
  if (n < 0 && errno == EINTR) continue;
  if (n <= 0) return -1;
  done += (size_t)n;
 }
 return 0;
}

int core_random(char out[65])
{
 unsigned char b[32];
 if (core_bytes(b, sizeof(b))) { out[0] = 0; return -1; }
 static const char hex[] = "0123456789abcdef";
 for (size_t i = 0; i < sizeof(b); i++) { out[i * 2] = hex[b[i] >> 4]; out[i * 2 + 1] = hex[b[i] & 15]; }
 out[64] = 0;
 explicit_bzero(b, sizeof(b));
 return 0;
}

/* Constant-time comparison of two 64-character secrets. */
int core_equal(const char *a, const char *b)
{
 if (!a || !b || strlen(a) != 64 || strlen(b) != 64) return 0;
 unsigned v = 0;
 for (int i = 0; i < 64; i++) v |= (unsigned char)a[i] ^ (unsigned char)b[i];
 return v == 0;
}

int core_mac_normalize(const char *in, char out[18])
{
 if (!in || strlen(in) != 17) return -1;
 for (int i = 0; i < 17; i++) {
  if (i % 3 == 2) { if (in[i] != ':') return -1; out[i] = ':'; continue; }
  if (!isxdigit((unsigned char)in[i])) return -1;
  out[i] = (char)tolower((unsigned char)in[i]);
 }
 out[17] = 0;
 unsigned first = (unsigned)strtoul(out, NULL, 16);
 return (first & 1) || !strcmp(out, "00:00:00:00:00:00") ? -1 : 0;
}

int core_ipv4(const char *s)
{
 struct in_addr a;
 char canonical[INET_ADDRSTRLEN];
 if (!s || strlen(s) > 15 || inet_pton(AF_INET, s, &a) != 1) return 0;
 return inet_ntop(AF_INET, &a, canonical, sizeof(canonical)) && !strcmp(canonical, s);
}

int core_text(const char *s, size_t max)
{
 if (!s || strlen(s) > max) return 0;
 for (const unsigned char *p = (const unsigned char *)s; *p; p++) if (*p < 0x20 || *p == 0x7f) return 0;
 return 1;
}

void core_close_gate(struct odac *c)
{
 RT(c)->online = 0;
 odac_platform_gate(c, 0);
}

int odac_open(struct odac *c)
{
 if (!c || c->runtime || c->db) return -1;
 if(odac_platform_gate(c,0)) return -1;
 const struct odac_config *k = &c->config;
 if (k->database[0] != '/' || k->max_sessions < 1 || k->max_sessions > ODAC_SESSIONS ||
     k->max_users < 1 || k->max_users > 2000 || k->max_devices < 1 || k->max_devices > 5000 ||
     k->max_audit < 10 || k->max_audit > 10000 || k->db_max_kib < 512 || k->db_max_kib > 65536 ||
     k->session_seconds < 60 || k->session_seconds > 604800 || k->wan_interval < 1 || k->wan_interval > 3600 ||
     k->upload_kbps < 0 || k->upload_kbps > ODAC_RATE_MAX || k->download_kbps < 0 || k->download_kbps > ODAC_RATE_MAX) return -1;
 c->runtime = calloc(1, sizeof(struct core_runtime));
 if (!c->runtime) return -1;
 RT(c)->now = core_time();
 RT(c)->base = c->config;
 RT(c)->default_devices = 3;
 /* The portal is enabled unless the administrator switches it off. */
 RT(c)->portal_enabled = 1;
 snprintf(RT(c)->wan_state, sizeof(RT(c)->wan_state), "UNKNOWN");
 /* The dummy hash makes unknown-user logins cost one real Argon2id verification. */
 if (db_init(c) || core_settings_load(c) || account_hash("odac-timing-equalizer", RT(c)->dummy_hash)) {
  odac_close(c);
  return -1;
 }
 odac_tick(c, RT(c)->now);
 return 0;
}

void odac_close(struct odac *c)
{
 if (!c) return;
 if (c->runtime) {
  session_revoke_all(c);
  core_close_gate(c);
  json_object_put(RT(c)->wan);
  json_object_put(RT(c)->live);
  explicit_bzero(c->runtime, sizeof(struct core_runtime));
  free(c->runtime);
  c->runtime = NULL;
 }
 if (c->db) { sqlite3_close(c->db); c->db = NULL; }
}

static void refresh_wan(struct odac *c, time_t now)
{
 struct core_runtime *r = RT(c);
 struct json_object *wan = odac_platform_wan(c, now);
 if (wan && !json_object_is_type(wan, json_type_object)) { json_object_put(wan); wan = NULL; }
 json_object_put(r->wan);
 r->wan = wan;
 r->last_wan = now;
 const char *state = core_str(wan, "state");
 static const char *const known[] = {"ONLINE", "REAUTHENTICATING", "AUTH_REQUIRED", "OFFLINE"};
 snprintf(r->wan_state, sizeof(r->wan_state), "UNKNOWN");
 for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++)
  if (!strcmp(state, known[i])) snprintf(r->wan_state, sizeof(r->wan_state), "%s", known[i]);
 if(strcmp(r->wan_logged,r->wan_state) || !r->last_wan_log || now-r->last_wan_log>=ODAC_WAN_LOG_SECONDS) {
  db_wan(c,r->wan_state); snprintf(r->wan_logged,sizeof(r->wan_logged),"%s",r->wan_state); r->last_wan_log=now;
 }
}

/* Order: retry failed revocations, observe WAN, drive the gate, expire sessions,
 * then refresh grant leases. Sessions survive WAN outages (RAM only); the gate
 * alone stops forwarding, and leases are re-applied when WAN returns. */
void odac_tick(struct odac *c, time_t now)
{
 if (!c || !c->runtime) return;
 struct core_runtime *r = RT(c);
 r->now = now;
 if (r->last_tick && now >= r->last_tick && now - r->last_tick < ODAC_TICK_SECONDS) return;
 r->last_tick = now;
 session_retry(c);
 if (!r->wan || now < r->last_wan || now - r->last_wan >= c->config.wan_interval) refresh_wan(c, now);
 int want = c->config.enabled && !strcmp(r->wan_state, "ONLINE") && !core_pending(c);
 int changed = want != r->online;
 int refresh = changed || !r->last_refresh || now < r->last_refresh || now - r->last_refresh >= ODAC_REFRESH_SECONDS;
 if (refresh) {
  if (want && odac_platform_gate(c, 1)) want = 0;
  if (!want) odac_platform_gate(c, 0);
  r->online = want;
 }
 session_expire(c);
 if (refresh) { r->last_refresh = now; policy_apply_portal(c); }
 if (!r->last_cleanup || now < r->last_cleanup || now - r->last_cleanup >= ODAC_CLEANUP_SECONDS) {
  r->last_cleanup = now;
  db_cleanup(c);
 }
}

struct json_object *odac_dispatch(struct odac *c, struct json_object *request)
{
 if (!c || !c->runtime || !c->db || !request || !json_object_is_type(request, json_type_object))
  return core_error("BAD_REQUEST", "Invalid request");
 RT(c)->now = core_time();
 struct json_object *params = NULL, *empty = NULL;
 if (json_object_object_get_ex(request, "params", &params)) {
  if (!json_object_is_type(params, json_type_object)) return core_error("BAD_REQUEST", "Parameters must be an object");
 } else params = empty = json_object_new_object();
 const char *scope = core_str(request, "scope");
 struct json_object *reply;
 if (!strcmp(scope, "portal")) reply = portal_dispatch(c, request, params);
 else if (!strcmp(scope, "admin")) reply = core_admin(c, core_str(request, "method"), params);
 else reply = core_error("FORBIDDEN", "Unknown request scope");
 json_object_put(empty);
 if(!core_bool(reply,"ok")) {
  const char *code=core_str(json_object_object_get(reply,"error"),"code");
  if(!strcmp(code,"DATABASE")) core_close_gate(c);
 }
 return reply;
}

time_t core_time(void)
{
#ifdef ODAC_TEST_HOOKS
 return (time_t)odac_test_clock;
#else
 return time(NULL);
#endif
}
int core_hex64(const char *s)
{
 if (!s || strlen(s)!=64) return 0;
 for (int i=0;i<64;i++) if (!isxdigit((unsigned char)s[i])) return 0;
 return 1;
}
