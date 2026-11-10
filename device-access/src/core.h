#ifndef ODAC_CORE_H
#define ODAC_CORE_H
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "odac.h"
#include <stddef.h>

/* Core-private limits. All RAM state is fixed-size. */
#define ODAC_SESSIONS 256
#define ODAC_PEERS 128
#define ODAC_GRANTS 512
#define ODAC_PENDING 64
#define ODAC_RATE_MAX 1000000
#define ODAC_TIME_MAX 4102444800LL
#define ODAC_TICK_SECONDS 5
#define ODAC_REFRESH_SECONDS 30 /* platform guard leases last 90 s */
#define ODAC_LIVE_SECONDS 10
#define ODAC_CLEANUP_SECONDS 3600
#define ODAC_RETENTION_SECONDS (30LL * 86400)
#define ODAC_WAN_LOG_SECONDS 60
#define ODAC_PAGE_DEFAULT 100
#define ODAC_PAGE_MAX 500
#define ODAC_WHITELIST_SECONDS 86400
#define ODAC_CSRF_SECONDS 3600
#define ODAC_PEER_WINDOW 60
#define ODAC_PEER_REQUESTS 60
#define ODAC_AUTH_WINDOW 300
#define ODAC_PEER_AUTH 10
#define ODAC_GLOBAL_WINDOW 60
#define ODAC_GLOBAL_AUTH 30
#define ODAC_DEVICE_NAME_MAX 128
#define ODAC_PASSWORD_MIN 6
#define ODAC_PASSWORD_MAX 128
/* Administrator-editable portal copy entries. */
#define ODAC_PORTAL_TEXTS 4
#define ODAC_BACKUP_MAX 8388608
#define ODAC_APPLICATION_ID 1329873219 /* 0x4F444143 "ODAC" */
#define ODAC_SCHEMA_VERSION 1

/* Fixed file locations. Host tests substitute these at compile time only. */
#ifdef ODAC_TEST_HOOKS
extern char odac_test_backup[256], odac_test_restore[256];
extern long long odac_test_clock;
#define ODAC_BACKUP_PATH odac_test_backup
#define ODAC_RESTORE_PATH odac_test_restore
#define ODAC_TRUSTED_UID getuid()
#else
#define ODAC_BACKUP_PATH "/tmp/odac-backup.json"
#define ODAC_RESTORE_PATH "/tmp/odac-restore.json"
#define ODAC_TRUSTED_UID 0
#endif

enum { POLICY_NONE = 0, POLICY_WHITELIST = 1, POLICY_BLACKLIST = 2, POLICY_EXPIRED = 3 };
enum { ENFORCE_REVOKE_FAILED = -2, ENFORCE_AUTH_FAILED = -1, ENFORCE_IDLE = 0, ENFORCE_GRANTED = 1 };

struct core_session { char token[65], mac[18]; int user_id, authorized; time_t expires; };
struct core_peer { char ip[16], csrf[65]; time_t seen, csrf_expires, window, auth_window; unsigned hits, auth_hits; };
struct core_runtime {
 struct core_session sessions[ODAC_SESSIONS];
 struct core_peer peers[ODAC_PEERS];
 char granted[ODAC_GRANTS][18];  /* MACs this process authorized on the platform */
 char pending[ODAC_PENDING][18]; /* failed revocations; forwarding stays closed */
 struct odac_config base;          /* configuration file values before DB overrides */
 struct json_object *wan, *live;
 time_t now, last_tick, last_wan, last_wan_log, last_refresh, last_cleanup, live_at, auth_window;
 int online, overflow, auth_hits, default_devices;
 /* Portal switch: 1 = clients authenticate as usual, 0 = the administrator
  * opened the controlled network and every device except blacklisted ones is
  * admitted without an account session. */
 int portal_enabled;
 char wan_state[20], wan_logged[20];
 char dummy_hash[256];
 char portal_text[ODAC_PORTAL_TEXTS][513];
};
#define RT(c) ((struct core_runtime *)(c)->runtime)

/* core.c */
time_t core_time(void);struct json_object *core_ok(struct json_object *data);
struct json_object *core_list(struct json_object *array, long long total);
struct json_object *core_error(const char *code, const char *message);
struct json_object *core_enforced(int rc, struct json_object *data);
struct json_object *core_parse(const char *text, size_t length, int depth);
const char *core_str(struct json_object *o, const char *key);
int core_bool(struct json_object *o, const char *key);
int core_has(struct json_object *o, const char *key);
int core_number(struct json_object *o, const char *key, long long min, long long max, int nullable);
long long core_ll(struct json_object *o, const char *key);
int core_keys(struct json_object *o, const char *const *allowed);
void core_addstr(struct json_object *o, const char *key, const char *value);
void core_addint(struct json_object *o, const char *key, long long value, int present);
int core_page(struct json_object *p, int *limit, int *offset);
int core_bytes(void *buffer, size_t length);
int core_random(char out[65]);
int core_hex64(const char *s);
int core_equal(const char *a, const char *b);
int core_mac_normalize(const char *in, char out[18]);
int core_ipv4(const char *s);
int core_text(const char *s, size_t max);
void core_close_gate(struct odac *c);

/* core_admin.c */
struct json_object *core_admin(struct odac *c, const char *method, struct json_object *p);

/* core_settings.c */
extern const char *const core_portal_text_keys[ODAC_PORTAL_TEXTS];
int core_setting_valid(const char *key, long long value);
int core_portal_text_valid(const char *key, const char *value);
struct json_object *core_portal_texts(struct odac *c);
int core_settings_load(struct odac *c);
struct json_object *core_settings(struct odac *c);
struct json_object *core_settings_update(struct odac *c, struct json_object *p);
struct json_object *core_backup(struct odac *c, struct json_object *p);
struct json_object *core_restore(struct odac *c, struct json_object *p);

/* db.c */
int db_init(struct odac *c);
sqlite3_stmt *db_prepare(struct odac *c, const char *sql);
int db_exec(struct odac *c, const char *sql);
long long db_scalar(struct odac *c, const char *sql, long long arg);
struct json_object *db_collect(sqlite3_stmt *s);
void db_audit(struct odac *c, const char *actor, const char *action, const char *target, const char *detail);
void db_wan(struct odac *c, const char *state);
void db_cleanup(struct odac *c);

/* account.c */
int account_username_valid(const char *s);
int account_hash_valid(const char *s);
int account_hash(const char *password, char out[256]);
struct json_object *account_register(struct odac *c, struct json_object *p, const char *mac, const char *ip);
struct json_object *account_login(struct odac *c, struct json_object *p, const char *mac);
struct json_object *account_admin(struct odac *c, const char *method, struct json_object *p);

/* session.c: RAM sessions and platform grant bookkeeping */
int grant_has(struct odac *c, const char *mac);
int grant_count(struct odac *c);
int grant_apply(struct odac *c, const char *mac, int seconds, int up, int down);
/* Like grant_apply but a single device failure never closes the gate. */
int grant_apply_soft(struct odac *c, const char *mac, int seconds, int up, int down);
int core_deauth(struct odac *c, const char *mac, int force);
int core_pending(struct odac *c);
void session_retry(struct odac *c);
struct core_session *session_by_mac(struct odac *c, const char *mac);
struct core_session *session_find(struct odac *c, const char *token, const char *mac);
void session_drop(struct core_session *s);
int session_count(struct odac *c, int user);
int session_valid(struct odac *c, struct core_session *s);
int session_apply(struct odac *c, struct core_session *s);
int session_revoke_user(struct odac *c, int user);
int session_revoke_all(struct odac *c);
int session_reconcile(struct odac *c, int user, int maximum);
void session_expire(struct odac *c);
struct json_object *session_create(struct odac *c, int user, int maximum, time_t expires, const char *mac);

/* policy.c */
int policy_kind(struct odac *c, const char *mac, time_t *expires);
const char *policy_name(int kind);
int policy_rates(struct odac *c, const char *mac, int user, int *up, int *down);
int policy_record(struct odac *c, const char *mac);
struct json_object *policy_live(struct odac *c, int max_age);
struct json_object *policy_live_find(struct odac *c, const char *mac);
int policy_enforce(struct odac *c, const char *mac);
void policy_expire(struct odac *c);
void policy_refresh(struct odac *c);
/* Re-apply admission after the administrator toggles the portal. */
void policy_apply_portal(struct odac *c);
struct json_object *policy_devices(struct odac *c, struct json_object *p);
struct json_object *policy_update(struct odac *c, struct json_object *p);
struct json_object *policy_disconnect(struct odac *c, struct json_object *p);

/* portal.c */
struct json_object *portal_dispatch(struct odac *c, struct json_object *r, struct json_object *p);
#endif
