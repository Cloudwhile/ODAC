#include "core.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct json_object *core_error(const char *code, const char *message)
{
 struct json_object *r = json_object_new_object(), *e = json_object_new_object();
 json_object_object_add(r, "ok", json_object_new_boolean(0));
 json_object_object_add(e, "code", json_object_new_string(code));
 json_object_object_add(e, "message", json_object_new_string(message));
 json_object_object_add(r, "error", e);
 return r;
}

struct json_object *core_ok(struct json_object *data)
{
 if (!data) return core_error("DATABASE", "Storage is unavailable");
 struct json_object *r = json_object_new_object();
 json_object_object_add(r, "ok", json_object_new_boolean(1));
 json_object_object_add(r, "data", data);
 return r;
}

struct json_object *core_list(struct json_object *array, long long total)
{
 struct json_object *r = core_ok(array);
 if (array && total >= 0) json_object_object_add(r, "total", json_object_new_int64(total));
 return r;
}

/* rc < 0 means a revocation failed: the change is committed and forwarding is closed. */
struct json_object *core_enforced(int rc, struct json_object *data)
{
 if (rc >= 0) return core_ok(data ? data : json_object_new_object());
 json_object_put(data);
 return core_error("ENFORCEMENT_PENDING", "The change was saved but access revocation failed; controlled network forwarding stays closed until it succeeds");
}

struct json_object *core_admin(struct odac *c,const char *method,struct json_object *p)
{
 if(!strcmp(method,"settings")) return core_ok(core_settings(c));
 if(!strcmp(method,"settings_update")) return core_settings_update(c,p);
 if(!strcmp(method,"backup")) return core_backup(c,p);
 if(!strcmp(method,"restore")) return core_restore(c,p);
 if(!strcmp(method,"devices")) return policy_devices(c,p);
 if(!strcmp(method,"device_update")) return policy_update(c,p);
 if(!strcmp(method,"disconnect")) return policy_disconnect(c,p);
 if(!strcmp(method,"users") || !strcmp(method,"registrations") || !strcmp(method,"approve") || !strcmp(method,"reject") || !strcmp(method,"user_update")) return account_admin(c,method,p);
 if(!strcmp(method,"wan")) return RT(c)->wan?core_ok(json_object_get(RT(c)->wan)):core_error("WAN","WAN status unavailable");
 if(!strcmp(method,"overview")) {
  struct json_object *o=json_object_new_object();
  json_object_object_add(o,"wan",RT(c)->wan?json_object_get(RT(c)->wan):json_object_new_object());
  struct json_object *live=policy_live(c,ODAC_LIVE_SECONDS);
  int online=0;
  if(live) for(size_t i=0;i<json_object_array_length(live);i++) if(core_bool(json_object_array_get_idx(live,i),"online")) online++;
  core_addint(o,"devices_online",online,1);
  core_addint(o,"devices_authorized",grant_count(c),1);
  core_addint(o,"registrations_pending",db_scalar(c,"SELECT count(*) FROM users WHERE status='PENDING'",0),1);
  core_addint(o,"users",db_scalar(c,"SELECT count(*) FROM users",0),1);
  core_addint(o,"whitelist",db_scalar(c,"SELECT count(*) FROM device_policies WHERE policy='WHITELIST'",0),1);
  core_addint(o,"blacklist",db_scalar(c,"SELECT count(*) FROM device_policies WHERE policy='BLACKLIST'",0),1);
  json_object_object_add(o,"upload_kbps",NULL);
  json_object_object_add(o,"download_kbps",NULL);
  json_object_object_add(o,"recent_events",db_collect(db_prepare(c,"SELECT at AS time,action AS type,detail AS username,target AS mac FROM audit_logs WHERE action='login' ORDER BY id DESC LIMIT 10")));
  json_object_object_add(o,"enabled",json_object_new_boolean(c->config.enabled));
  return core_ok(o);
 }
 if(!strcmp(method,"audit")) {
  int limit,offset; if(core_page(p,&limit,&offset)) return core_error("INVALID_INPUT","Invalid page");
  sqlite3_stmt *s=db_prepare(c,"SELECT at AS time,actor,action,target,detail FROM audit_logs ORDER BY id DESC LIMIT ? OFFSET ?");
  if(s) { sqlite3_bind_int(s,1,limit); sqlite3_bind_int(s,2,offset); }
  return core_list(db_collect(s),db_scalar(c,"SELECT count(*) FROM audit_logs",0));
 }
 return core_error("METHOD","Unknown admin method");
}
static int record_valid(struct json_object *o,int kind)
{
 static const char *const userkeys[]={"username","password_hash","status","created_at","expires_at","max_devices","upload_kbps","download_kbps",NULL};
 static const char *const devicekeys[]={"mac","alias","first_seen",NULL};
 static const char *const bindingkeys[]={"mac","username",NULL};
 static const char *const policykeys[]={"mac","policy","expires_at","upload_kbps","download_kbps",NULL};
 const char *const *keys=kind==0?userkeys:kind==1?devicekeys:kind==2?bindingkeys:policykeys;
 if(!json_object_is_type(o,json_type_object) || !core_keys(o,keys)) return 0;
 for(int i=0;keys[i];i++) if(!core_has(o,keys[i])) return 0;
 if(kind==0) {
  const char *status=core_str(o,"status");
  if(!account_username_valid(core_str(o,"username")) || !account_hash_valid(core_str(o,"password_hash")) ||
   (strcmp(status,"ACTIVE") && strcmp(status,"PENDING") && strcmp(status,"REJECTED") && strcmp(status,"DISABLED") && strcmp(status,"BANNED")) ||
   !core_number(o,"created_at",0,ODAC_TIME_MAX,0) || !core_number(o,"max_devices",1,ODAC_SESSIONS,0)) return 0;
 }
 if(kind!=0) { char mac[18]; if(core_mac_normalize(core_str(o,"mac"),mac) || strcmp(mac,core_str(o,"mac"))) return 0; }
 if(kind==1) return core_has(o,"alias") && json_object_is_type(json_object_object_get(o,"alias"),json_type_string) && core_text(core_str(o,"alias"),128) && core_number(o,"first_seen",0,ODAC_TIME_MAX,0);
 if(kind==2) return account_username_valid(core_str(o,"username"));
 if(kind==3) { const char *policy=core_str(o,"policy"); if(strcmp(policy,"NONE") && strcmp(policy,"WHITELIST") && strcmp(policy,"BLACKLIST")) return 0; }
 return core_number(o,"expires_at",0,ODAC_TIME_MAX,1) && core_number(o,"upload_kbps",0,ODAC_RATE_MAX,1) && core_number(o,"download_kbps",0,ODAC_RATE_MAX,1);
}
static int import_rows(struct odac *c,struct json_object *a,int kind)
{
 const char *sql[]={
 "INSERT INTO users(username,password_hash,status,created_at,expires_at,max_devices,upload_kbps,download_kbps) VALUES(?,?,?,?,?,?,?,?)",
 "INSERT INTO devices(mac,alias,first_seen) VALUES(?,?,?)",
 "INSERT INTO device_bindings(mac,user_id) VALUES(?,(SELECT id FROM users WHERE username=?))",
 "INSERT INTO device_policies(mac,policy,expires_at,upload_kbps,download_kbps) VALUES(?,?,?,?,?)"};
 const char *keys[][9]={{"username","password_hash","status","created_at","expires_at","max_devices","upload_kbps","download_kbps",NULL},{"mac","alias","first_seen",NULL},{"mac","username",NULL},{"mac","policy","expires_at","upload_kbps","download_kbps",NULL}};
 sqlite3_stmt *s=db_prepare(c,sql[kind]); if(!s) return -1;
 int failed=0;
 for(size_t i=0;i<json_object_array_length(a);i++) {
  struct json_object *o=json_object_array_get_idx(a,i);
  for(int j=0;keys[kind][j];j++) {
   struct json_object *v=json_object_object_get(o,keys[kind][j]);
   if(!v) sqlite3_bind_null(s,j+1);
   else if(json_object_is_type(v,json_type_int)) sqlite3_bind_int64(s,j+1,json_object_get_int64(v));
   else sqlite3_bind_text(s,j+1,json_object_get_string(v),-1,SQLITE_TRANSIENT);
  }
  if(sqlite3_step(s)!=SQLITE_DONE || (kind==2 && db_scalar(c,"SELECT count(*) FROM device_bindings WHERE user_id IS NULL",0)!=0)) { failed=1; break; }
  sqlite3_reset(s); sqlite3_clear_bindings(s);
 }
 sqlite3_finalize(s); return failed?-1:0;
}
struct json_object *core_restore(struct odac *c,struct json_object *p)
{
 static const char *const keys[]={"path",NULL};
 if(!core_keys(p,keys) || strcmp(core_str(p,"path"),ODAC_RESTORE_PATH)) return core_error("RESTORE_PATH","Only the fixed restore path is accepted");
 struct stat before,st;
 if(lstat(ODAC_RESTORE_PATH,&before) || !S_ISREG(before.st_mode) || before.st_nlink!=1 || before.st_uid!=ODAC_TRUSTED_UID || (before.st_mode&077) || before.st_size<1 || before.st_size>ODAC_BACKUP_MAX) return core_error("RESTORE_FILE","Unsafe restore file");
 int fd=open(ODAC_RESTORE_PATH,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
 if(fd<0) return core_error("RESTORE_FILE","Cannot open restore file");
 int valid=!fstat(fd,&st) && st.st_dev==before.st_dev && st.st_ino==before.st_ino && S_ISREG(st.st_mode) && st.st_uid==ODAC_TRUSTED_UID && st.st_nlink==1 && !(st.st_mode&077) && st.st_size==before.st_size;
 char *text=valid?malloc((size_t)st.st_size+1):NULL; size_t done=0;
 if(text) while(done<(size_t)st.st_size) { ssize_t n=read(fd,text+done,(size_t)st.st_size-done); if(n<0 && errno==EINTR) continue; if(n<=0) break; done+=(size_t)n; }
 char extra; if(!text || done!=(size_t)st.st_size || read(fd,&extra,1)!=0) valid=0;
 close(fd);
 struct json_object *o=valid?core_parse(text,done,16):NULL; free(text);
 static const char *const top[]={"format","version","created_at","settings","users","devices","bindings","policies",NULL};
 if(!o || !core_keys(o,top) || strcmp(core_str(o,"format"),"odac-backup") || !core_number(o,"version",1,1,0) || !core_number(o,"created_at",0,ODAC_TIME_MAX,0)) { json_object_put(o); return core_error("RESTORE_INVALID","Invalid backup document"); }
 struct json_object *settings=json_object_object_get(o,"settings"); valid=json_object_is_type(settings,json_type_object);
 if(valid) { json_object_object_foreach(settings,key,v) {
  if(json_object_is_type(v,json_type_string)) { if(!core_portal_text_valid(key,json_object_get_string(v)) || strlen(json_object_get_string(v))!=(size_t)json_object_get_string_len(v)) valid=0; }
  else if(!json_object_is_type(v,json_type_int) || !core_setting_valid(key,json_object_get_int64(v))) valid=0;
 } }
 const char *required[]={"max_users","max_devices","max_sessions","session_seconds","upload_kbps","download_kbps","max_audit",NULL};
 for(int i=0;valid && required[i];i++) if(!core_has(settings,required[i])) valid=0;
 if(!core_has(settings,"default_devices")) valid=0;
 /* A backup taken before the portal switch existed restores with it enabled. */
 if(valid && !core_has(settings,"portal_enabled")) core_addint(settings,"portal_enabled",1,1);
 const char *names[]={"users","devices","bindings","policies"}; struct json_object *arrays[4];
 for(int i=0;i<4;i++) {
  arrays[i]=json_object_object_get(o,names[i]);
  if(!json_object_is_type(arrays[i],json_type_array)) { valid=0; continue; }
  size_t count=json_object_array_length(arrays[i]);
  long long cap=i==0?core_ll(settings,"max_users"):i==2?core_ll(settings,"max_users")*core_ll(settings,"max_devices"):core_ll(settings,"max_devices");
  if(count>(size_t)cap) valid=0;
  for(size_t j=0;valid && j<count;j++) if(!record_valid(json_object_array_get_idx(arrays[i],j),i)) valid=0;
 }
 if(!valid) { json_object_put(o); return core_error("RESTORE_INVALID","Invalid backup records"); }
 if(db_exec(c,"BEGIN IMMEDIATE")) { json_object_put(o); return core_error("DATABASE","Cannot begin restore"); }
 valid=!db_exec(c,"DELETE FROM registration_requests; DELETE FROM device_bindings; DELETE FROM device_policies; DELETE FROM devices; DELETE FROM users; DELETE FROM settings; DELETE FROM audit_logs;");
 for(int i=0;valid && i<4;i++) if(import_rows(c,arrays[i],i)) valid=0;
 sqlite3_stmt *s=valid?db_prepare(c,"INSERT INTO settings(id,value) VALUES(1,?)"):NULL;
 if(s) { sqlite3_bind_text(s,1,json_object_to_json_string_ext(settings,JSON_C_TO_STRING_PLAIN),-1,SQLITE_TRANSIENT); valid=sqlite3_step(s)==SQLITE_DONE; } else valid=0;
 sqlite3_finalize(s);
 if(!valid || db_exec(c,"COMMIT")) { db_exec(c,"ROLLBACK"); json_object_put(o); return core_error("RESTORE_INVALID","Restore transaction rejected"); }
 int revoked=session_revoke_all(c); memset(RT(c)->peers,0,sizeof(RT(c)->peers));
 if(core_settings_load(c)) { core_close_gate(c); json_object_put(o); return core_error("DATABASE","Restored settings unavailable"); }
 struct json_object *result=json_object_new_object();
 for(int i=0;i<4;i++) core_addint(result,names[i],(long long)json_object_array_length(arrays[i]),1);
 json_object_put(o);
 if(unlink(ODAC_RESTORE_PATH)) { json_object_put(result); return core_error("RESTORE_CLEANUP","Restore committed but upload removal failed"); }
 return core_enforced(revoked,result);
}
