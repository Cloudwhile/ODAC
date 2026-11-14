#include "core.h"
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct setting { const char *key; size_t offset; int min,max; };
#define FIELD(k,a,b) {#k,offsetof(struct odac_config,k),a,b}
static const struct setting fields[]={
 FIELD(max_users,1,2000),FIELD(max_devices,1,5000),FIELD(max_sessions,1,ODAC_SESSIONS),
 FIELD(session_seconds,60,604800),FIELD(upload_kbps,0,ODAC_RATE_MAX),
 FIELD(download_kbps,0,ODAC_RATE_MAX),FIELD(max_audit,10,10000),
 FIELD(wan_interval,5,30),FIELD(db_max_kib,512,65536)
};
int core_setting_valid(const char *key,long long value)
{
 if(!strcmp(key,"default_devices")) return value>=1 && value<=ODAC_SESSIONS;
 if(!strcmp(key,"portal_enabled")) return value==0 || value==1;
 for(size_t i=0;i<sizeof(fields)/sizeof(fields[0]);i++) if(!strcmp(key,fields[i].key)) return value>=fields[i].min && value<=fields[i].max;
 return 0;
}
/* Portal copy the administrator may replace; an empty value restores the built-in text. */
const char *const core_portal_text_keys[ODAC_PORTAL_TEXTS]={"portal_title","portal_subtitle","portal_register_hint","portal_footer"};
static const size_t portal_text_max[ODAC_PORTAL_TEXTS]={64,128,256,512};
static int portal_text_index(const char *key)
{
 for(int i=0;i<ODAC_PORTAL_TEXTS;i++) if(!strcmp(key,core_portal_text_keys[i])) return i;
 return -1;
}
int core_portal_text_valid(const char *key,const char *value)
{
 int i=portal_text_index(key);
 return i>=0 && value && core_text(value,portal_text_max[i]);
}
struct json_object *core_portal_texts(struct odac *c)
{
 struct json_object *o=json_object_new_object();
 for(int i=0;i<ODAC_PORTAL_TEXTS;i++) core_addstr(o,core_portal_text_keys[i],RT(c)->portal_text[i]);
 return o;
}
/* Every settings value is an integer tunable or a portal text string. */
static int setting_entry_valid(const char *key,struct json_object *v)
{
 if(portal_text_index(key)>=0) return json_object_is_type(v,json_type_string) && strlen(json_object_get_string(v))==(size_t)json_object_get_string_len(v) && core_portal_text_valid(key,json_object_get_string(v));
 return json_object_is_type(v,json_type_int) && core_setting_valid(key,json_object_get_int64(v));
}
static void add_portal_texts(struct odac *c,struct json_object *o)
{
 for(int i=0;i<ODAC_PORTAL_TEXTS;i++) core_addstr(o,core_portal_text_keys[i],RT(c)->portal_text[i]);
}
struct json_object *core_settings(struct odac *c)
{
 struct json_object *o=json_object_new_object();
 json_object_object_add(o,"daemon_available",json_object_new_boolean(1));
 json_object_object_add(o,"config_enabled",json_object_new_boolean(c->config.enabled));
 for(size_t i=0;i<sizeof(fields)/sizeof(fields[0]);i++) core_addint(o,fields[i].key,*(int *)((char *)&c->config+fields[i].offset),1);
 core_addint(o,"default_devices",RT(c)->default_devices,1);
 core_addstr(o,"interface",c->config.interface);
 core_addstr(o,"gateway",c->config.gateway);
 core_addstr(o,"upstream",c->config.upstream);
 core_addstr(o,"portal_hostname",c->config.portal_hostname);
 core_addint(o,"portal_https_port",c->config.portal_https_port,1);
 core_addint(o,"portal_port",c->config.portal_port,1);
 core_addstr(o,"tls_cert",c->config.tls_cert);
 core_addint(o,"portal_enabled",RT(c)->portal_enabled,1);
 add_portal_texts(c,o);
 json_object_object_add(o,"enabled",json_object_new_boolean(c->config.enabled)); return o;
}
int core_settings_load(struct odac *c)
{
 sqlite3_stmt *s=db_prepare(c,"SELECT value FROM settings WHERE id=1");
 if(!s) return -1;
 int rc=sqlite3_step(s); struct json_object *o=NULL;
 if(rc==SQLITE_ROW) o=core_parse((const char *)sqlite3_column_text(s,0),(size_t)sqlite3_column_bytes(s,0),8);
 sqlite3_finalize(s);
 if(rc==SQLITE_DONE) return 0;
 if(!o) return -1;
 int valid=1;
 json_object_object_foreach(o,key,v) { if(!setting_entry_valid(key,v)) valid=0; }
 if(valid) {
  for(size_t i=0;i<sizeof(fields)/sizeof(fields[0]);i++) if(core_has(o,fields[i].key)) *(int *)((char *)&c->config+fields[i].offset)=(int)core_ll(o,fields[i].key);
  if(core_has(o,"default_devices")) RT(c)->default_devices=(int)core_ll(o,"default_devices");
  /* The portal switch defaults to enabled when an older database has no value. */
  RT(c)->portal_enabled=core_has(o,"portal_enabled")?(int)core_ll(o,"portal_enabled"):1;
  for(int i=0;i<ODAC_PORTAL_TEXTS;i++) snprintf(RT(c)->portal_text[i],sizeof(RT(c)->portal_text[i]),"%s",core_str(o,core_portal_text_keys[i]));
 } else { json_object_put(o); return -1; }
 json_object_put(o); return db_apply_limit(c);
}
struct json_object *core_settings_update(struct odac *c,struct json_object *p)
{
 if(core_has(p,"enabled")) return core_error("READ_ONLY","enabled is controlled by system configuration");
 json_object_object_foreach(p,key,v) {
  if(!setting_entry_valid(key,v)) return core_error("INVALID_INPUT","Invalid or unsupported setting");
 }
 struct json_object *o=json_object_new_object();
 for(size_t i=0;i<sizeof(fields)/sizeof(fields[0]);i++) core_addint(o,fields[i].key,*(int *)((char *)&c->config+fields[i].offset),1);
 core_addint(o,"default_devices",RT(c)->default_devices,1);
 core_addint(o,"portal_enabled",RT(c)->portal_enabled,1);
 add_portal_texts(c,o);
 json_object_object_foreach(p,setting_key,value) { json_object_object_add(o,setting_key,json_object_get(value)); }
 long long users=db_scalar(c,"SELECT count(*) FROM users",0), devices=db_scalar(c,"SELECT count(*) FROM devices",0);
 if(users<0 || devices<0) { json_object_put(o); return core_error("DATABASE","Cannot inspect capacity"); }
 if(core_ll(o,"max_users")<users || core_ll(o,"max_devices")<devices) { json_object_put(o); return core_error("CAPACITY","Caps cannot be lower than stored counts"); }
 int switched=core_has(p,"portal_enabled")&&(int)core_ll(p,"portal_enabled")!=RT(c)->portal_enabled;
 sqlite3_stmt *s=db_prepare(c,"INSERT INTO settings(id,value) VALUES(1,?) ON CONFLICT(id) DO UPDATE SET value=excluded.value"); int rc=SQLITE_ERROR;
 if(s) { sqlite3_bind_text(s,1,json_object_to_json_string_ext(o,JSON_C_TO_STRING_PLAIN),-1,SQLITE_TRANSIENT); rc=sqlite3_step(s); }
 sqlite3_finalize(s); json_object_put(o);
 if(rc!=SQLITE_DONE || core_settings_load(c)) { core_close_gate(c); return core_error("DATABASE","Settings could not be saved"); }
 int enforce=session_reconcile(c,0,c->config.max_sessions); policy_refresh(c); db_cleanup(c);
 /* Switching the portal changes who may pass: re-apply the device policies so
  * admitted devices are granted and revoked devices stay revoked. */
 if(switched) { policy_apply_portal(c); db_audit(c,"admin","portal_enabled",RT(c)->portal_enabled?"on":"off",""); }
 if(core_pending(c)) enforce=-1;
 return core_enforced(enforce,core_settings(c));
}
struct json_object *core_backup(struct odac *c,struct json_object *p)
{
 static const char *const keys[]={NULL};
 if(!core_keys(p,keys)) return core_error("INVALID_INPUT","Backup takes no parameters");
 struct json_object *o=json_object_new_object(); core_addstr(o,"format","odac-backup"); core_addint(o,"version",1,1); core_addint(o,"created_at",RT(c)->now,1);
 struct json_object *settings=json_object_new_object();
 for(size_t i=0;i<sizeof(fields)/sizeof(fields[0]);i++) core_addint(settings,fields[i].key,*(int *)((char *)&c->config+fields[i].offset),1);
 core_addint(settings,"default_devices",RT(c)->default_devices,1);
 core_addint(settings,"portal_enabled",RT(c)->portal_enabled,1);
 add_portal_texts(c,settings);
 json_object_object_add(o,"settings",settings);
 const char *names[]={"users","devices","bindings","policies"};
 const char *sql[]={
 "SELECT username,password_hash,status,created_at,expires_at,max_devices,upload_kbps,download_kbps FROM users ORDER BY id",
 "SELECT mac,alias,first_seen FROM devices ORDER BY mac",
 "SELECT b.mac,u.username FROM device_bindings b JOIN users u ON u.id=b.user_id ORDER BY b.mac,u.username",
 "SELECT mac,policy,expires_at,upload_kbps,download_kbps FROM device_policies ORDER BY mac"};
 if(db_exec(c,"BEGIN")) { json_object_put(o); return core_error("DATABASE","Backup snapshot failed"); }
 int valid=1;
 for(int i=0;i<4;i++) { struct json_object *a=db_collect(db_prepare(c,sql[i])); if(!a) { valid=0; break; } json_object_object_add(o,names[i],a); }
 if(db_exec(c,valid?"COMMIT":"ROLLBACK")) valid=0;
 if(!valid) { json_object_put(o); core_close_gate(c); return core_error("DATABASE","Backup snapshot failed"); }
 const char *text=json_object_to_json_string_ext(o,JSON_C_TO_STRING_PLAIN); size_t length=strlen(text);
 char path[300]; snprintf(path,sizeof(path),"%s.XXXXXX",ODAC_BACKUP_PATH);
 int fd=length<=ODAC_BACKUP_MAX?mkstemp(path):-1; size_t done=0;
 if(fd>=0 && !fchmod(fd,0600)) {
  while(done<length) { ssize_t n=write(fd,text+done,length-done); if(n<0 && errno==EINTR) continue; if(n<=0) break; done+=(size_t)n; }
 }
 valid=fd>=0 && done==length && !fsync(fd);
 if(fd>=0 && close(fd)) valid=0;
 if(valid && rename(path,ODAC_BACKUP_PATH)) valid=0;
 if(!valid) unlink(path);
 json_object_put(o);
 if(!valid) return core_error("BACKUP_FAILED","Backup could not be written");
 struct json_object *result=json_object_new_object(); core_addstr(result,"path",ODAC_BACKUP_PATH); core_addint(result,"size",(long long)length,1); core_addint(result,"created_at",RT(c)->now,1); return core_ok(result);
}
