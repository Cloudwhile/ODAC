#include "core.h"
#include <argon2.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* Serialized event loop bounds peak KDF memory. 8 MiB / 3 passes is a router
 * resource compromise below desktop recommendations; long passwords and
 * bounded online attempts are required, and database theft remains a risk. */
int account_hash(const char *password, char out[256])
{
 char salt[65];
 if(strlen(password)<ODAC_PASSWORD_MIN || strlen(password)>128 || core_random(salt)) return -1;
 return argon2id_hash_encoded(3,8192,1,password,strlen(password),salt,32,32,out,256)==ARGON2_OK ? 0 : -1;
}
int account_username_valid(const char *s)
{
 size_t n=strlen(s);
 if(n<3 || n>32) return 0;
 for(size_t i=0;i<n;i++) if(!isalnum((unsigned char)s[i]) && s[i]!='_' && s[i]!='-' && s[i]!='.') return 0;
 return 1;
}
struct json_object *account_register(struct odac *c, struct json_object *p, const char *mac, const char *ip)
{
 const char *name=core_str(p,"username"),*pw=core_str(p,"password"),*note=core_str(p,"note");
 char hash[256],request_id[65],hostname[ODAC_DEVICE_NAME_MAX];
 hostname[0]=0;
 if(!account_username_valid(name) || strlen(pw)<ODAC_PASSWORD_MIN || strlen(pw)>128 || (core_has(p,"confirm_password") && strcmp(pw,core_str(p,"confirm_password"))) || strlen(note)>256) return core_error("INVALID_INPUT","Use a 3-32 character username and matching 6-128 character passwords");
 /* One account per device. The MAC comes from the verified neighbor table, so the
  * page cannot reuse an identity; IP and device name are recorded for review. */
 sqlite3_stmt *owner=db_prepare(c,"SELECT count(*) FROM registration_requests WHERE mac=?");
 long long registered=-1;
 if(owner) { sqlite3_bind_text(owner,1,mac,-1,SQLITE_TRANSIENT); if(sqlite3_step(owner)==SQLITE_ROW) registered=sqlite3_column_int64(owner,0); }
 sqlite3_finalize(owner);
 if(registered<0) return core_error("DATABASE","Storage unavailable");
 if(registered) return core_error("DEVICE_REGISTERED","This device already registered an account");
 if(odac_platform_hostname(c,mac,ip,hostname,sizeof(hostname))) hostname[0]=0;
 long long count=db_scalar(c,"SELECT count(*) FROM users",0);
 if(count<0 || count>=c->config.max_users) return core_error("CAPACITY","Registration capacity reached");
 /* The request row keeps a random unique key internally; it is never shown to the client. */
 if(account_hash(pw,hash) || core_random(request_id)) return core_error("CRYPTO_FAILED","Password protection unavailable");
 if(db_exec(c,"BEGIN IMMEDIATE")) return core_error("DATABASE","Storage unavailable");
 sqlite3_stmt *s=db_prepare(c,"INSERT INTO users(username,password_hash,status,created_at,max_devices) VALUES(?,?,'PENDING',?,?)");
 int rc=SQLITE_ERROR;
 if(s) { sqlite3_bind_text(s,1,name,-1,SQLITE_TRANSIENT); sqlite3_bind_text(s,2,hash,-1,SQLITE_TRANSIENT); sqlite3_bind_int64(s,3,RT(c)->now); sqlite3_bind_int(s,4,RT(c)->default_devices); rc=sqlite3_step(s); }
 sqlite3_finalize(s); memset(hash,0,sizeof(hash));
 if(rc!=SQLITE_DONE) { db_exec(c,"ROLLBACK"); return core_error("REGISTRATION_FAILED","Registration could not be accepted"); }
 int id=(int)sqlite3_last_insert_rowid(c->db);
 s=db_prepare(c,"INSERT INTO registration_requests(user_id,receipt,note,mac,ip,hostname,created_at) VALUES(?,?,?,?,?,?,?)");
 if(s) { sqlite3_bind_int(s,1,id); sqlite3_bind_text(s,2,request_id,-1,SQLITE_TRANSIENT); sqlite3_bind_text(s,3,note,-1,SQLITE_TRANSIENT); sqlite3_bind_text(s,4,mac,-1,SQLITE_TRANSIENT); sqlite3_bind_text(s,5,ip,-1,SQLITE_TRANSIENT); sqlite3_bind_text(s,6,hostname,-1,SQLITE_TRANSIENT); sqlite3_bind_int64(s,7,RT(c)->now); rc=sqlite3_step(s); } else rc=SQLITE_ERROR;
 sqlite3_finalize(s); explicit_bzero(request_id,sizeof(request_id));
 if(rc!=SQLITE_DONE || db_exec(c,"COMMIT")) { db_exec(c,"ROLLBACK"); return core_error("DATABASE","Registration could not be saved"); }
 db_audit(c,name,"register",name,"");
 struct json_object *o=json_object_new_object(); core_addstr(o,"status","PENDING"); return core_ok(o);
}
struct json_object *account_login(struct odac *c,struct json_object *p,const char *mac)
{
 const char *pw=core_str(p,"password"),*name=core_str(p,"username");
 if(strlen(pw)>128 || !account_username_valid(name)) return core_error("LOGIN_FAILED","Invalid credentials or unavailable account");
 sqlite3_stmt *s=db_prepare(c,"SELECT id,password_hash,status,max_devices,expires_at FROM users WHERE username=?");
 int id=0,max=0; time_t expires=0; char hash[256],status[16];
 snprintf(hash,sizeof(hash),"%s",RT(c)->dummy_hash); status[0]=0;
 if(s) { sqlite3_bind_text(s,1,name,-1,SQLITE_TRANSIENT); if(sqlite3_step(s)==SQLITE_ROW) { id=sqlite3_column_int(s,0); snprintf(hash,sizeof(hash),"%s",sqlite3_column_text(s,1)); snprintf(status,sizeof(status),"%s",sqlite3_column_text(s,2)); max=sqlite3_column_int(s,3); expires=sqlite3_column_int64(s,4); } }
 sqlite3_finalize(s);
 int verified=argon2id_verify(hash,pw,strlen(pw))==ARGON2_OK; memset(hash,0,sizeof(hash));
 if(!verified || !id) return core_error("LOGIN_FAILED","Invalid credentials or unavailable account");
 /* Credentials are proven: a pending, rejected, disabled, banned or expired account is told it lacks access. */
 if(strcmp(status,"ACTIVE") || (expires && expires<=RT(c)->now)) return core_error("NO_ACCESS","This account has no access");
 time_t policy_expiry=0; int kind=policy_kind(c,mac,&policy_expiry);
 if(kind<0 || kind==POLICY_BLACKLIST || kind==POLICY_EXPIRED) return core_error("DEVICE_BLOCKED","This device is blocked");
 if(policy_record(c,mac)) return core_error("DATABASE","Device could not be recorded");
 struct json_object *result=session_create(c,id,max,expires,mac);
 if(!core_bool(result,"ok")) return result;
 int saved=1;
 s=db_prepare(c,"INSERT OR IGNORE INTO device_bindings(mac,user_id) VALUES(?,?)");
 if(s) { sqlite3_bind_text(s,1,mac,-1,SQLITE_TRANSIENT); sqlite3_bind_int(s,2,id); saved=sqlite3_step(s)==SQLITE_DONE; } else saved=0; sqlite3_finalize(s);
 s=db_prepare(c,"UPDATE users SET last_login=? WHERE id=?");
 if(s) { sqlite3_bind_int64(s,1,RT(c)->now); sqlite3_bind_int(s,2,id); if(sqlite3_step(s)!=SQLITE_DONE) saved=0; } else saved=0; sqlite3_finalize(s);
 if(!saved) { session_revoke_user(c,id); core_close_gate(c); json_object_put(result); return core_error("DATABASE","Login could not be recorded"); }
 db_audit(c,name,"login",mac,name); return result;
}
static struct json_object *approval(struct odac *c,const char *method,struct json_object *p)
{
 struct json_object *ids=NULL;
 if(!json_object_object_get_ex(p,"ids",&ids) || !json_object_is_type(ids,json_type_array) || !json_object_array_length(ids) || json_object_array_length(ids)>200) return core_error("INVALID_INPUT","Supply a bounded list of registration IDs");
 for(size_t i=0;i<json_object_array_length(ids);i++) { struct json_object *v=json_object_array_get_idx(ids,i); if(!json_object_is_type(v,json_type_int) || json_object_get_int64(v)<1 || json_object_get_int64(v)>2147483647) return core_error("INVALID_INPUT","Invalid account ID"); }
 if(db_exec(c,"BEGIN IMMEDIATE")) return core_error("DATABASE","Storage unavailable");
 sqlite3_stmt *s=db_prepare(c,"UPDATE users SET status=? WHERE id=? AND status='PENDING'");
 int failed=!s;
 for(size_t i=0;s && i<json_object_array_length(ids);i++) {
  sqlite3_bind_text(s,1,!strcmp(method,"approve")?"ACTIVE":"REJECTED",-1,SQLITE_STATIC);
  sqlite3_bind_int(s,2,json_object_get_int(json_object_array_get_idx(ids,i)));
  if(sqlite3_step(s)!=SQLITE_DONE || sqlite3_changes(c->db)!=1) { failed=1; break; }
  sqlite3_reset(s); sqlite3_clear_bindings(s);
 }
 sqlite3_finalize(s);
 if(failed || db_exec(c,"COMMIT")) { db_exec(c,"ROLLBACK"); return core_error("INVALID_STATE","Every account must be pending"); }
 db_audit(c,"admin",method,"registrations",""); return core_ok(json_object_new_object());
}
struct json_object *account_admin(struct odac *c,const char *method,struct json_object *p)
{
 if(!strcmp(method,"users") || !strcmp(method,"registrations")) {
  int limit,offset; if(core_page(p,&limit,&offset)) return core_error("INVALID_INPUT","Invalid page");
  int users=!strcmp(method,"users");
  sqlite3_stmt *q=db_prepare(c,users?"SELECT u.id,u.username,u.status,u.created_at,u.last_login,u.expires_at,u.max_devices,u.upload_kbps,u.download_kbps,(SELECT count(*) FROM device_bindings b WHERE b.user_id=u.id) AS device_count FROM users u ORDER BY u.id DESC LIMIT ? OFFSET ?":"SELECT u.id,u.username,u.status,r.created_at,r.note,r.mac,r.ip AS ipv4,r.hostname FROM users u JOIN registration_requests r ON r.user_id=u.id WHERE u.status='PENDING' ORDER BY u.id LIMIT ? OFFSET ?");
  if(q) { sqlite3_bind_int(q,1,limit); sqlite3_bind_int(q,2,offset); }
  return core_list(db_collect(q),db_scalar(c,users?"SELECT count(*) FROM users":"SELECT count(*) FROM users WHERE status='PENDING'",0));
 }
 if(!strcmp(method,"approve") || !strcmp(method,"reject")) return approval(c,method,p);
 int id=(int)core_ll(p,"id"); const char *action=core_str(p,"action");
 if(!core_number(p,"id",1,2147483647,0)) return core_error("INVALID_INPUT","Invalid account ID");
 sqlite3_stmt *s=db_prepare(c,"SELECT status FROM users WHERE id=?"); char status[16]={0};
 if(s) { sqlite3_bind_int(s,1,id); if(sqlite3_step(s)==SQLITE_ROW) snprintf(status,sizeof(status),"%s",sqlite3_column_text(s,0)); } sqlite3_finalize(s);
 if(!status[0]) return core_error("NOT_FOUND","Account not found");
 const char *next=NULL,*sql=NULL; char hash[256]={0};
 if(!strcmp(action,"ban")) next="BANNED";
 else if(!strcmp(action,"unban") && !strcmp(status,"BANNED")) next="ACTIVE";
 else if(!strcmp(action,"disable")) next="DISABLED";
 else if(!strcmp(action,"enable") && !strcmp(status,"DISABLED")) next="ACTIVE";
 else if(!strcmp(action,"delete")) sql="DELETE FROM users WHERE id=?";
 else if(!strcmp(action,"disconnect")) sql="";
 else if(!strcmp(action,"reset_password")) { if(account_hash(core_str(p,"password"),hash)) return core_error("INVALID_INPUT","Password must contain 6-128 characters"); sql="UPDATE users SET password_hash=? WHERE id=?"; }
 else if(!strcmp(action,"update")) {
  if((core_has(p,"expires_at") && !core_number(p,"expires_at",0,ODAC_TIME_MAX,1)) || (core_has(p,"max_devices") && !core_number(p,"max_devices",1,ODAC_SESSIONS,0)) || (core_has(p,"upload_kbps") && !core_number(p,"upload_kbps",0,ODAC_RATE_MAX,1)) || (core_has(p,"download_kbps") && !core_number(p,"download_kbps",0,ODAC_RATE_MAX,1))) return core_error("INVALID_INPUT","Invalid account policy");
  sql="UPDATE users SET expires_at=CASE WHEN ? THEN ? ELSE expires_at END,max_devices=CASE WHEN ? THEN ? ELSE max_devices END,upload_kbps=CASE WHEN ? THEN ? ELSE upload_kbps END,download_kbps=CASE WHEN ? THEN ? ELSE download_kbps END WHERE id=?";
 }
 if(next) sql="UPDATE users SET status=? WHERE id=?";
 if(!sql) return core_error("INVALID_STATE","Unsupported account action or transition");
 /* Revoke before acknowledging any policy mutation. Failure closes WAN gate. */
 if(session_revoke_user(c,id)) return core_error("ENFORCEMENT_FAILED","Revocation failed; forwarding is closed");
 int rc=SQLITE_DONE;
 if(*sql) {
  s=db_prepare(c,sql); if(!s) return core_error("DATABASE","Storage unavailable");
  if(!strcmp(action,"update")) {
   const char *keys[]={"expires_at","max_devices","upload_kbps","download_kbps"};
   for(int i=0;i<4;i++) { struct json_object *v=NULL; int present=json_object_object_get_ex(p,keys[i],&v); sqlite3_bind_int(s,1+i*2,present); if(v) sqlite3_bind_int64(s,2+i*2,json_object_get_int64(v)); else sqlite3_bind_null(s,2+i*2); } sqlite3_bind_int(s,9,id);
  } else if(next || hash[0]) { sqlite3_bind_text(s,1,next?next:hash,-1,SQLITE_TRANSIENT); sqlite3_bind_int(s,2,id); }
  else sqlite3_bind_int(s,1,id);
  rc=sqlite3_step(s); sqlite3_finalize(s);
 }
 memset(hash,0,sizeof(hash));
 if(rc!=SQLITE_DONE) return core_error("DATABASE","Account change could not be saved");
 char target[32]; snprintf(target,sizeof(target),"user:%d",id); db_audit(c,"admin",action,target,"");
 return core_ok(json_object_new_object());
}

int account_hash_valid(const char *s)
{
 return s && strlen(s)>10 && strlen(s)<256 && !strncmp(s,"$argon2id$",10);
}
