#include "core.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

int policy_record(struct odac *c,const char *mac)
{
 sqlite3_stmt *s=db_prepare(c,"SELECT 1 FROM devices WHERE mac=?"); int found=0;
 if(s) { sqlite3_bind_text(s,1,mac,-1,SQLITE_TRANSIENT); found=sqlite3_step(s)==SQLITE_ROW; } sqlite3_finalize(s);
 if(found) return 0;
 int n=db_scalar(c,"SELECT count(*) FROM devices",0); if(n<0 || n>=c->config.max_devices) return -1;
 s=db_prepare(c,"INSERT INTO devices(mac,first_seen) VALUES(?,?)"); int rc=SQLITE_ERROR;
 if(s) { sqlite3_bind_text(s,1,mac,-1,SQLITE_TRANSIENT); sqlite3_bind_int64(s,2,RT(c)->now); rc=sqlite3_step(s); } sqlite3_finalize(s); return rc==SQLITE_DONE?0:-1;
}
int policy_kind(struct odac *c,const char *mac,time_t *expires)
{
 sqlite3_stmt *s=db_prepare(c,"SELECT policy,expires_at FROM device_policies WHERE mac=?"); int kind=0;
 *expires=0;
 if(!s) { core_close_gate(c); return -1; }
 sqlite3_bind_text(s,1,mac,-1,SQLITE_TRANSIENT);
 int rc=sqlite3_step(s);
 if(rc==SQLITE_ROW) {
  *expires=sqlite3_column_int64(s,1);
  const char *p=(const char *)sqlite3_column_text(s,0);
  if(!strcmp(p,"BLACKLIST")) kind=2;
  else if(!strcmp(p,"WHITELIST")) kind=1;
  else if(strcmp(p,"NONE")) kind=-1;
  if(*expires && *expires<=RT(c)->now) kind=kind==POLICY_WHITELIST?POLICY_EXPIRED:POLICY_NONE;
 }
 sqlite3_finalize(s); if(rc!=SQLITE_ROW && rc!=SQLITE_DONE) { core_close_gate(c); return -1; } return kind;
}
int policy_rates(struct odac *c,const char *mac,int user,int *up,int *down)
{
 *up=c->config.upload_kbps; *down=c->config.download_kbps;
 const char *sqls[]={"SELECT upload_kbps,download_kbps FROM users WHERE id=?","SELECT upload_kbps,download_kbps FROM device_policies WHERE mac=?"};
 for(int i=0;i<2;i++) {
  sqlite3_stmt *s=db_prepare(c,sqls[i]); if(!s) return -1;
  if(i) sqlite3_bind_text(s,1,mac,-1,SQLITE_TRANSIENT); else sqlite3_bind_int(s,1,user);
  int rc=sqlite3_step(s);
  if(rc==SQLITE_ROW) { if(sqlite3_column_type(s,0)!=SQLITE_NULL) *up=sqlite3_column_int(s,0); if(sqlite3_column_type(s,1)!=SQLITE_NULL) *down=sqlite3_column_int(s,1); }
  sqlite3_finalize(s); if(rc!=SQLITE_ROW && rc!=SQLITE_DONE) return -1;
 }
 return 0;
}
const char *policy_name(int kind)
{
 switch(kind) { case POLICY_NONE:return "NONE"; case POLICY_WHITELIST:return "WHITELIST"; case POLICY_BLACKLIST:return "BLACKLIST"; case POLICY_EXPIRED:return "EXPIRED"; default:return "UNKNOWN"; }
}
struct json_object *policy_live(struct odac *c,int max_age)
{
 if(!RT(c)->live || RT(c)->now<RT(c)->live_at || RT(c)->now-RT(c)->live_at>=max_age) {
  struct json_object *a=odac_platform_devices(c);
  if(!a || !json_object_is_type(a,json_type_array)) { json_object_put(a); core_close_gate(c); return NULL; }
  json_object_put(RT(c)->live); RT(c)->live=a; RT(c)->live_at=RT(c)->now;
 }
 return RT(c)->live;
}
struct json_object *policy_live_find(struct odac *c,const char *mac)
{
 struct json_object *a=policy_live(c,ODAC_LIVE_SECONDS);
 if(!a) return NULL;
 for(size_t i=0;i<json_object_array_length(a);i++) { struct json_object *o=json_object_array_get_idx(a,i); if(!strcmp(core_str(o,"mac"),mac)) return o; }
 return NULL;
}
int policy_enforce(struct odac *c,const char *mac)
{
 time_t expiry=0; int kind=policy_kind(c,mac,&expiry);
 struct core_session *s=session_by_mac(c,mac);
 if(kind<0) { core_close_gate(c); core_deauth(c,mac,1); return ENFORCE_AUTH_FAILED; }
 if(kind==POLICY_BLACKLIST || kind==POLICY_EXPIRED) return core_deauth(c,mac,1)<0?ENFORCE_REVOKE_FAILED:ENFORCE_IDLE;
 /* With the portal disabled the controlled network is open: every device
  * that is not blacklisted is admitted without an account session. */
 if(!RT(c)->portal_enabled) {
  int open_up,open_down;
  if(policy_rates(c,mac,0,&open_up,&open_down)) { core_close_gate(c); return ENFORCE_AUTH_FAILED; }
  return grant_apply_soft(c,mac,c->config.session_seconds,open_up,open_down)==0?ENFORCE_GRANTED:ENFORCE_AUTH_FAILED;
 }
 if(s) return session_apply(c,s);
 if(kind==POLICY_WHITELIST) {
  /* A valid whitelist entry grants access without any account session.
   * Rate precedence stays device over account over system default. */
  int up,down;
  if(policy_rates(c,mac,0,&up,&down)) { core_close_gate(c); return ENFORCE_AUTH_FAILED; }
  return grant_apply(c,mac,c->config.session_seconds,up,down)==0?ENFORCE_GRANTED:ENFORCE_AUTH_FAILED;
 }
 return core_deauth(c,mac,1)<0?ENFORCE_REVOKE_FAILED:ENFORCE_IDLE;
}
void policy_expire(struct odac *c)
{
 for(int i=0;i<ODAC_GRANTS;i++) if(RT(c)->granted[i][0]) {
  char mac[18]; snprintf(mac,sizeof(mac),"%s",RT(c)->granted[i]); policy_enforce(c,mac);
 }
}
void policy_refresh(struct odac *c)
{
 session_expire(c);
 for(int i=0;i<ODAC_SESSIONS;i++) if(RT(c)->sessions[i].mac[0]) policy_enforce(c,RT(c)->sessions[i].mac);
 policy_expire(c);
}
/* With the portal disabled the controlled network is open: every device that
 * is not blacklisted is admitted without an account session. Blacklisted and
 * expired devices stay revoked, and a device that cannot be granted is left
 * out on its own instead of closing the gate for everyone else. */
void policy_apply_portal(struct odac *c)
{
 if(RT(c)->portal_enabled) { policy_refresh(c); return; }
 struct json_object *live=policy_live(c,ODAC_LIVE_SECONDS);
 if(!live) { core_close_gate(c); return; }
 for(size_t i=0;i<json_object_array_length(live);i++) {
  struct json_object *o=json_object_array_get_idx(live,i);
  char mac[18]; if(core_mac_normalize(core_str(o,"mac"),mac)) continue;
  time_t expiry=0; int kind=policy_kind(c,mac,&expiry);
  if(kind<0) { core_close_gate(c); continue; }
  if(kind==POLICY_BLACKLIST || kind==POLICY_EXPIRED) { core_deauth(c,mac,1); continue; }
  int up,down;
  if(policy_rates(c,mac,0,&up,&down)) { core_close_gate(c); continue; }
  grant_apply_soft(c,mac,c->config.session_seconds,up,down);
 }
}
struct json_object *policy_devices(struct odac *c,struct json_object *p)
{
 int limit,offset;
 if(core_page(p,&limit,&offset)) return core_error("INVALID_INPUT","Invalid page");
 struct json_object *live=policy_live(c,ODAC_LIVE_SECONDS);
 if(!live) return core_error("PLATFORM","Client discovery unavailable");
 for(size_t i=0;i<json_object_array_length(live);i++) {
  char mac[18]; if(!core_mac_normalize(core_str(json_object_array_get_idx(live,i),"mac"),mac) && policy_record(c,mac)) return core_error("CAPACITY","Device record capacity reached");
 }
 sqlite3_stmt *s=db_prepare(c,"SELECT d.mac,d.alias,d.first_seen,COALESCE(p.policy,'NONE') AS policy,p.expires_at,p.upload_kbps,p.download_kbps,(SELECT u.username FROM device_bindings b JOIN users u ON u.id=b.user_id WHERE b.mac=d.mac LIMIT 1) AS username FROM devices d LEFT JOIN device_policies p ON p.mac=d.mac ORDER BY d.first_seen DESC,d.mac LIMIT ? OFFSET ?");
 if(s) { sqlite3_bind_int(s,1,limit); sqlite3_bind_int(s,2,offset); }
 struct json_object *a=db_collect(s);
 if(!a) return core_error("DATABASE","Device query failed");
 for(size_t i=0;i<json_object_array_length(a);i++) {
  struct json_object *o=json_object_array_get_idx(a,i); const char *mac=core_str(o,"mac");
  struct json_object *current=policy_live_find(c,mac);
  if(current) { json_object_object_foreach(current,key,v) { if(strcmp(key,"mac") && !core_has(o,key)) json_object_object_add(o,key,json_object_get(v)); } }
  json_object_object_add(o,"granted",json_object_new_boolean(RT(c)->online && grant_has(c,mac)));
 }
 return core_list(a,db_scalar(c,"SELECT count(*) FROM devices",0));
}
struct json_object *policy_update(struct odac *c,struct json_object *p)
{
 const char *input=core_str(p,"mac"),*kind=core_str(p,"policy"); char mac[18];
 if(core_mac_normalize(input,mac)) return core_error("INVALID_INPUT","Invalid MAC address");
 for(int i=0;i<18;i++) mac[i]=(char)tolower((unsigned char)input[i]);
 if(core_has(p,"policy") && strcmp(kind,"NONE") && strcmp(kind,"WHITELIST") && strcmp(kind,"BLACKLIST")) return core_error("INVALID_INPUT","Invalid device policy");
 if((core_has(p,"alias") && (!json_object_is_type(json_object_object_get(p,"alias"),json_type_string) || !core_text(core_str(p,"alias"),128))) || (core_has(p,"expires_at") && !core_number(p,"expires_at",0,ODAC_TIME_MAX,1)) || (core_has(p,"upload_kbps") && !core_number(p,"upload_kbps",0,ODAC_RATE_MAX,1)) || (core_has(p,"download_kbps") && !core_number(p,"download_kbps",0,ODAC_RATE_MAX,1))) return core_error("INVALID_INPUT","Invalid device settings");
 if(policy_record(c,mac)) return core_error("CAPACITY","Device record capacity reached");
 struct core_session *active=session_by_mac(c,mac);
 if(active) session_drop(active);
 if(core_deauth(c,mac,1)) return core_error("ENFORCEMENT_FAILED","Revocation failed; forwarding is closed");
 if(db_exec(c,"BEGIN IMMEDIATE")) return core_error("DATABASE","Storage unavailable");
 sqlite3_stmt *s=db_prepare(c,"INSERT OR IGNORE INTO device_policies(mac) VALUES(?)"); int rc=SQLITE_ERROR;
 if(s) { sqlite3_bind_text(s,1,mac,-1,SQLITE_TRANSIENT); rc=sqlite3_step(s); } sqlite3_finalize(s);
 if(rc==SQLITE_DONE) {
  s=db_prepare(c,"UPDATE device_policies SET policy=CASE WHEN ? THEN ? ELSE policy END,expires_at=CASE WHEN ? THEN ? ELSE expires_at END,upload_kbps=CASE WHEN ? THEN ? ELSE upload_kbps END,download_kbps=CASE WHEN ? THEN ? ELSE download_kbps END WHERE mac=?");
  if(s) {
   sqlite3_bind_int(s,1,core_has(p,"policy")); sqlite3_bind_text(s,2,kind,-1,SQLITE_TRANSIENT);
   const char *keys[]={"expires_at","upload_kbps","download_kbps"};
   for(int i=0;i<3;i++) { struct json_object *v=NULL; sqlite3_bind_int(s,3+i*2,json_object_object_get_ex(p,keys[i],&v)); if(v) sqlite3_bind_int64(s,4+i*2,json_object_get_int64(v)); else sqlite3_bind_null(s,4+i*2); }
   sqlite3_bind_text(s,9,mac,-1,SQLITE_TRANSIENT); rc=sqlite3_step(s);
  } else rc=SQLITE_ERROR; sqlite3_finalize(s);
 }
 if(rc==SQLITE_DONE && core_has(p,"alias")) {
  s=db_prepare(c,"UPDATE devices SET alias=? WHERE mac=?"); if(s) { sqlite3_bind_text(s,1,core_str(p,"alias"),-1,SQLITE_TRANSIENT); sqlite3_bind_text(s,2,mac,-1,SQLITE_TRANSIENT); rc=sqlite3_step(s); } else rc=SQLITE_ERROR; sqlite3_finalize(s);
 }
 if(rc!=SQLITE_DONE || db_exec(c,"COMMIT")) { db_exec(c,"ROLLBACK"); return core_error("DATABASE","Device change could not be saved"); }
 int enforced=policy_enforce(c,mac); db_audit(c,"admin","device_update",mac,""); if(enforced<0) return core_error("ENFORCEMENT_FAILED","Policy saved; forwarding closed"); return core_ok(json_object_new_object());
}
struct json_object *policy_disconnect(struct odac *c,struct json_object *p)
{
 char mac[18]; if(core_mac_normalize(core_str(p,"mac"),mac)) return core_error("INVALID_INPUT","Invalid MAC address");
 struct core_session *s=session_by_mac(c,mac); if(s) session_drop(s);
 return core_enforced(core_deauth(c,mac,1),json_object_new_object());
}
