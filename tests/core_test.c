#include "core.h"
#include <assert.h>
#include <fcntl.h>
#include <json-c/json_util.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int gate,auths,deauth_failure,passes;
static const char *wan_state="ONLINE";
static char last_auth[18];
static int live_device;
int odac_platform_gate(struct odac *c,int online) { (void)c; gate=online; return 0; }
int odac_platform_auth(struct odac *c,const char *mac,int seconds,int upload,int download)
{
 (void)c; assert(seconds>0); assert(upload>=0 && download>=0); assert(gate); auths++; snprintf(last_auth,sizeof(last_auth),"%s",mac); return 0;
}
int odac_platform_deauth(struct odac *c,const char *mac) { (void)c; (void)mac; return deauth_failure?-1:0; }
struct json_object *odac_platform_wan(struct odac *c,time_t now)
{
 (void)c; (void)now; struct json_object *o=json_object_new_object(); core_addstr(o,"state",wan_state); return o;
}
struct json_object *odac_platform_client(struct odac *c,const char *ip)
{
 (void)c; if(strcmp(ip,"192.0.2.10") && strcmp(ip,"192.0.2.11")) return NULL;
 struct json_object *o=json_object_new_object(); core_addstr(o,"mac",!strcmp(ip,"192.0.2.10")?"02:00:00:00:00:10":"02:00:00:00:00:11"); return o;
}
int odac_platform_hostname(struct odac *c,const char *mac,const char *ip,char *out,size_t capacity)
{ (void)c; (void)mac; (void)ip; if(out && capacity) out[0]=0; return -1; }
struct json_object *odac_platform_devices(struct odac *c)
{
 (void)c;
 struct json_object *a=json_object_new_array();
 /* The open-portal test needs a discoverable device; other tests want none. */
 if(!live_device) return a;
 struct json_object *d=json_object_new_object();
 core_addstr(d,"mac","02:00:00:00:00:10"); core_addstr(d,"ipv4","192.0.2.10");
 json_object_object_add(d,"online",json_object_new_boolean(1));
 json_object_array_add(a,d); return a;
}
static struct json_object *call(struct odac *c,const char *scope,const char *method,const char *params,const char *token)
{
 struct json_object *r=json_object_new_object(); core_addstr(r,"scope",scope); core_addstr(r,"method",method); core_addstr(r,"peer","192.0.2.10");
 json_object_object_add(r,"https",json_object_new_boolean(1)); core_addstr(r,"token",token);
 json_object_object_add(r,"params",core_parse(params,strlen(params),16));
 struct json_object *reply=odac_dispatch(c,r); json_object_put(r); return reply;
}
static void check(int condition,const char *name)
{
 if(!condition) { fprintf(stderr,"FAIL: %s\n",name); exit(1); } passes++; printf("PASS: %s\n",name);
}
static struct json_object *data(struct json_object *r) { return json_object_object_get(r,"data"); }
/* Parse a JSON literal; the length always comes from the literal itself. */
static struct json_object *parse(const char *text) { return core_parse(text,strlen(text),8); }
static void json_compat(void);
static void portal_text_restart(void);
static void schema_upgrade(void);
static void portal_switch(void);
static void upload(const char *text)
{
 unlink(odac_test_restore); int fd=open(odac_test_restore,O_WRONLY|O_CREAT|O_EXCL,0600); assert(fd>=0);
 size_t n=strlen(text); assert(write(fd,text,n)==(ssize_t)n); assert(!close(fd));
}
int main(void)
{
 char dir[]="/tmp/odac-core-XXXXXX"; assert(mkdtemp(dir)); struct odac c={0};
 snprintf(c.config.database,sizeof(c.config.database),"%s/db",dir);
 snprintf(odac_test_backup,sizeof(odac_test_backup),"%s/backup.json",dir); snprintf(odac_test_restore,sizeof(odac_test_restore),"%s/restore.json",dir);
 c.config.enabled=1; c.config.max_users=20; c.config.max_devices=30; c.config.max_sessions=10; c.config.session_seconds=3600; c.config.max_audit=100; c.config.db_max_kib=2048; c.config.wan_interval=5;
 assert(!odac_open(&c)); char csrf[65],token[65],params[1024]; struct json_object *r;
 r=call(&c,"portal","csrf","{}",NULL); snprintf(csrf,sizeof(csrf),"%s",core_str(data(r),"csrf")); json_object_put(r);
 snprintf(params,sizeof(params),"{\"username\":\"shorty\",\"password\":\"12345\",\"csrf\":\"%s\"}",csrf);
 r=call(&c,"portal","register",params,NULL); check(!core_bool(r,"ok") && !strcmp(core_str(json_object_object_get(r,"error"),"code"),"INVALID_INPUT"),"registration rejects passwords under 6 characters"); json_object_put(r);
 snprintf(params,sizeof(params),"{\"username\":\"alice\",\"password\":\"password10\",\"csrf\":\"%s\",\"mac\":\"02:00:00:00:00:ff\"}",csrf);
 r=call(&c,"portal","register",params,NULL);
 check(core_bool(r,"ok") && !strcmp(core_str(data(r),"status"),"PENDING") && !core_has(data(r),"receipt"),"registration creates a pending account without a client-visible receipt"); json_object_put(r);
 r=call(&c,"portal","registration-status","{\"receipt\":\"x\"}",NULL); check(!core_bool(r,"ok"),"receipt lookup is not a portal method"); json_object_put(r);
 r=call(&c,"portal","login",params,NULL); check(!core_bool(r,"ok") && !strcmp(core_str(json_object_object_get(r,"error"),"code"),"NO_ACCESS") && auths==0,"pending account is told it has no access"); json_object_put(r);
 snprintf(params,sizeof(params),"{\"username\":\"bob\",\"password\":\"password10\",\"csrf\":\"%s\"}",csrf);
 r=call(&c,"portal","register",params,NULL); check(!core_bool(r,"ok") && !strcmp(core_str(json_object_object_get(r,"error"),"code"),"DEVICE_REGISTERED"),"a device can register only one account"); json_object_put(r);
 snprintf(params,sizeof(params),"{\"username\":\"alice\",\"password\":\"password10\",\"csrf\":\"%s\"}",csrf);
 r=call(&c,"admin","approve","{\"ids\":[1]}",NULL); assert(core_bool(r,"ok")); json_object_put(r);
 r=call(&c,"portal","login",params,NULL); snprintf(token,sizeof(token),"%s",core_str(data(r),"token"));
 check(core_bool(r,"ok") && core_hex64(token),"approval allows login returning a token");
 check(!strcmp(last_auth,"02:00:00:00:00:10"),"portal ignores client supplied MAC"); json_object_put(r);
 r=call(&c,"portal","status","{}",token); check(core_bool(data(r),"authenticated") && core_hex64(core_str(data(r),"csrf")) && !core_has(data(r),"wan") && json_object_is_type(json_object_object_get(data(r),"text"),json_type_object),"status supplies CSRF and portal text without upstream details"); json_object_put(r);
 r=call(&c,"portal","login","{\"username\":\"alice\",\"password\":\"password10\"}",NULL); check(!core_bool(r,"ok") && !strcmp(core_str(json_object_object_get(r,"error"),"code"),"CSRF"),"login requires peer CSRF"); json_object_put(r);
 r=call(&c,"admin","settings_update","{\"wan_interval\":10,\"db_max_kib\":4096}",NULL); check(core_bool(r,"ok") && c.config.wan_interval==10 && c.config.db_max_kib==4096,"settings applies WAN interval and database limit"); json_object_put(r);
 check(!core_settings_load(&c) && c.config.wan_interval==10 && c.config.db_max_kib==4096,"operational settings persist in the database");
 r=call(&c,"admin","settings_update","{\"wan_interval\":4,\"db_max_kib\":2048}",NULL); check(!core_bool(r,"ok") && c.config.db_max_kib==4096,"invalid WAN interval rejects the entire update"); json_object_put(r);
 r=call(&c,"admin","settings_update","{\"wan_interval\":31}",NULL); check(!core_bool(r,"ok"),"WAN interval rejects values above 30"); json_object_put(r);
 r=call(&c,"admin","settings_update","{\"db_max_kib\":511}",NULL); check(!core_bool(r,"ok"),"database limit rejects values below 512"); json_object_put(r);
 r=call(&c,"admin","settings_update","{\"db_max_kib\":65537}",NULL); check(!core_bool(r,"ok"),"database limit rejects values above 65536"); json_object_put(r);
 r=call(&c,"portal","settings_update","{\"wan_interval\":15}",NULL); check(!core_bool(r,"ok") && c.config.wan_interval==10,"portal clients cannot modify administrative settings"); json_object_put(r);
 check(db_scalar(&c,"PRAGMA max_page_count",0)*db_scalar(&c,"PRAGMA page_size",0)==4096*1024,"database growth limit applies immediately");
 r=call(&c,"admin","settings_update","{\"wan_interval\":5,\"db_max_kib\":2048}",NULL); check(core_bool(r,"ok"),"restore operational settings for subsequent timing tests"); json_object_put(r);
 r=call(&c,"admin","settings_update","{\"enabled\":false}",NULL); check(!core_bool(r,"ok") && !strcmp(core_str(json_object_object_get(r,"error"),"code"),"READ_ONLY"),"enabled is read only"); json_object_put(r);
 r=call(&c,"admin","settings_update","{\"portal_title\":\"\\u5bbf\\u820d\\u7f51\\u7edc\",\"portal_footer\":\"\\u8054\\u7cfb 8021\"}",NULL); check(core_bool(r,"ok") || (fprintf(stderr,"DEBUG %s\n",json_object_to_json_string(r)),0),"administrator can replace portal text"); json_object_put(r);
 r=call(&c,"portal","status","{}",token); check(!strcmp(core_str(json_object_object_get(data(r),"text"),"portal_title"),"\xe5\xae\xbf\xe8\x88\x8d\xe7\xbd\x91\xe7\xbb\x9c") && !strcmp(core_str(json_object_object_get(data(r),"text"),"portal_subtitle"),""),"portal status serves the administrator text and leaves unset text empty"); json_object_put(r);
 r=call(&c,"admin","settings_update","{\"portal_title\":\"line\\nbreak\"}",NULL); check(!core_bool(r,"ok"),"portal text rejects control characters"); json_object_put(r);
 r=call(&c,"admin","settings_update","{\"portal_title\":5}",NULL); check(!core_bool(r,"ok"),"portal text must be a string"); json_object_put(r);
 r=call(&c,"admin","settings","{}",NULL);
 check(core_bool(r,"ok") && core_has(data(r),"portal_hostname") && core_has(data(r),"wan_interval") && core_has(data(r),"tls_cert"),"settings expose the read-only network fields the UI shows");
 json_object_put(r);
 r=call(&c,"admin","overview","{}",NULL);
 check(core_bool(r,"ok") && json_object_is_type(json_object_object_get(data(r),"wan"),json_type_object) && core_has(data(r),"devices_online") &&
  core_has(data(r),"devices_authorized") && core_has(data(r),"registrations_pending") && core_has(data(r),"whitelist") &&
  core_has(data(r),"blacklist") && json_object_is_type(json_object_object_get(data(r),"recent_events"),json_type_array),"overview exposes every dashboard field");
 json_object_put(r);
 r=call(&c,"admin","users","{}",NULL);
 check(core_bool(r,"ok") && json_object_array_length(data(r))==1 && core_has(json_object_array_get_idx(data(r),0),"device_count"),"user list reports the device count");
 json_object_put(r);
 r=call(&c,"admin","audit","{}",NULL);
 check(core_bool(r,"ok") && json_object_array_length(data(r))>0 && core_has(json_object_array_get_idx(data(r),0),"actor") &&
  core_has(json_object_array_get_idx(data(r),0),"time") && core_has(json_object_array_get_idx(data(r),0),"detail"),"audit rows carry actor, time and detail");
 json_object_put(r);
 r=call(&c,"admin","backup","{}",NULL); assert(core_bool(r,"ok")); check(json_object_object_length(data(r))==3,"backup replies only with file metadata"); json_object_put(r);
 struct json_object *backup=json_object_from_file(odac_test_backup); assert(backup); const char *serialized=json_object_to_json_string_ext(backup,JSON_C_TO_STRING_PLAIN);
 check(!strstr(serialized,token) && !core_has(backup,"sessions") && !strcmp(core_str(json_object_object_get(backup,"settings"),"portal_title"),"\xe5\xae\xbf\xe8\x88\x8d\xe7\xbd\x91\xe7\xbb\x9c"),"backup excludes session secrets and keeps portal text");
 struct stat st; assert(!stat(odac_test_backup,&st)); check(!(st.st_mode&077),"backup has owner only permissions");
 struct json_object *users=json_object_object_get(backup,"users"),*user=json_object_array_get_idx(users,0);
 core_addstr(user,"username","bad!name"); upload(json_object_to_json_string_ext(backup,JSON_C_TO_STRING_PLAIN));
 snprintf(params,sizeof(params),"{\"path\":\"%s\"}",odac_test_restore);
 r=call(&c,"admin","restore",params,NULL); check(!core_bool(r,"ok") && db_scalar(&c,"SELECT count(*) FROM users WHERE username='alice'",0)==1 && session_find(&c,token,"02:00:00:00:00:10"),"tampered restore leaves database and session unchanged"); json_object_put(r);
 core_addstr(user,"username","restored");
 struct json_object *binding=json_object_array_get_idx(json_object_object_get(backup,"bindings"),0); assert(binding); core_addstr(binding,"username","restored");
 /* An invalid foreign key fails after DELETEs, proving transaction rollback. */
 core_addstr(binding,"mac","02:00:00:00:00:99"); upload(json_object_to_json_string_ext(backup,JSON_C_TO_STRING_PLAIN));
 r=call(&c,"admin","restore",params,NULL); check(!core_bool(r,"ok") && db_scalar(&c,"SELECT count(*) FROM users WHERE username='alice'",0)==1,"failed import rolls back deletion of live data"); json_object_put(r);
 core_addstr(binding,"mac","02:00:00:00:00:10"); upload(json_object_to_json_string_ext(backup,JSON_C_TO_STRING_PLAIN));
 r=call(&c,"admin","restore",params,NULL); check(core_bool(r,"ok") && db_scalar(&c,"SELECT count(*) FROM users WHERE username='restored'",0)==1 && db_scalar(&c,"SELECT count(*) FROM users WHERE username='alice'",0)==0,"valid restore replaces stored accounts"); json_object_put(r);
 r=call(&c,"portal","status","{}",token); check(!core_bool(data(r),"authenticated"),"restore invalidates old token"); json_object_put(r);
 unlink(odac_test_restore); assert(!symlink(odac_test_backup,odac_test_restore));
 r=call(&c,"admin","restore",params,NULL); check(!core_bool(r,"ok"),"symlink restore rejected"); json_object_put(r); unlink(odac_test_restore);
 int fd=open(odac_test_restore,O_CREAT|O_WRONLY,0600); assert(fd>=0); assert(!ftruncate(fd,ODAC_BACKUP_MAX+1)); assert(!close(fd));
 r=call(&c,"admin","restore",params,NULL); check(!core_bool(r,"ok"),"oversized restore rejected"); json_object_put(r); unlink(odac_test_restore);
 r=call(&c,"admin","device_update","{\"mac\":\"02:00:00:00:00:10\",\"policy\":\"BLACKLIST\"}",NULL); assert(core_bool(r,"ok")); json_object_put(r);
 r=call(&c,"portal","csrf","{}",NULL); snprintf(csrf,sizeof(csrf),"%s",core_str(data(r),"csrf")); json_object_put(r);
 snprintf(params,sizeof(params),"{\"username\":\"restored\",\"password\":\"password10\",\"csrf\":\"%s\"}",csrf);
 r=call(&c,"portal","login",params,NULL); check(!core_bool(r,"ok") && !strcmp(core_str(json_object_object_get(r,"error"),"code"),"DEVICE_BLOCKED"),"blocked device cannot log in"); json_object_put(r);
 r=call(&c,"admin","device_update","{\"mac\":\"02:00:00:00:00:10\",\"policy\":\"NONE\"}",NULL); assert(core_bool(r,"ok")); json_object_put(r);
 auths=0; r=call(&c,"admin","device_update","{\"mac\":\"02:00:00:00:00:10\",\"policy\":\"WHITELIST\"}",NULL); assert(core_bool(r,"ok")); json_object_put(r);
 check(auths==1 && !strcmp(last_auth,"02:00:00:00:00:10"),"whitelisted device is authorized without an account session");
 r=call(&c,"portal","status","{}",NULL); check(!core_bool(data(r),"authenticated"),"whitelisted device still has no portal session"); json_object_put(r);
 r=call(&c,"admin","device_update","{\"mac\":\"02:00:00:00:00:10\",\"policy\":\"BLACKLIST\"}",NULL); assert(core_bool(r,"ok")); json_object_put(r);
 auths=0; odac_test_clock+=5; odac_tick(&c,(time_t)odac_test_clock); check(auths==0,"blacklisted device is not re-authorized on refresh");
 r=call(&c,"admin","device_update","{\"mac\":\"02:00:00:00:00:10\",\"policy\":\"NONE\"}",NULL); assert(core_bool(r,"ok")); json_object_put(r);
 r=call(&c,"portal","login",params,NULL); assert(core_bool(r,"ok")); snprintf(token,sizeof(token),"%s",core_str(data(r),"token")); json_object_put(r);
 snprintf(params,sizeof(params),"{\"token\":\"%s\",\"mac\":\"02:00:00:00:00:11\"}",token);
 r=call(&c,"portal","status",params,NULL); check(!core_bool(data(r),"authenticated"),"portal ignores token inside params"); json_object_put(r);
 deauth_failure=1; r=call(&c,"admin","disconnect","{\"mac\":\"02:00:00:00:00:10\"}",NULL);
 check(!core_bool(r,"ok") && !gate && core_pending(&c),"failed deauth queues retry and closes forwarding"); json_object_put(r);
 deauth_failure=0; odac_test_clock+=5; odac_tick(&c,(time_t)odac_test_clock); check(!core_pending(&c),"tick retries failed deauth");
 wan_state="UNRECOGNIZED"; odac_test_clock+=5; odac_tick(&c,(time_t)odac_test_clock); check(!gate && !RT(&c)->online,"unknown WAN state closes forwarding");
 snprintf(params,sizeof(params),"{\"username\":\"restored\",\"password\":\"password10\",\"csrf\":\"%s\"}",csrf);
 odac_test_clock+=ODAC_CSRF_SECONDS; r=call(&c,"portal","login",params,NULL);
 check(!core_bool(r,"ok") && !strcmp(core_str(json_object_object_get(r,"error"),"code"),"CSRF"),"expired CSRF cannot authorize mutation"); json_object_put(r);
 odac_test_clock+=ODAC_PEER_WINDOW;
 for(int i=0;i<ODAC_PEER_REQUESTS;i++) { r=call(&c,"portal","status","{}",NULL); assert(core_bool(r,"ok")); json_object_put(r); }
 r=call(&c,"portal","status","{}",NULL); check(!core_bool(r,"ok") && !strcmp(core_str(json_object_object_get(r,"error"),"code"),"RATE_LIMIT"),"peer request rate is bounded"); json_object_put(r);
 json_object_put(backup); odac_close(&c); unlink(odac_test_backup); unlink(c.config.database); assert(!rmdir(dir));
 json_compat();
 portal_text_restart();
 schema_upgrade();
 portal_switch();
 printf("%d passed\n",passes); return 0;
}
/* The administrator can open the controlled network without touching the
 * firewall configuration: every device except blacklisted ones is admitted. */
static void portal_switch(void)
{
 char dir[]="/tmp/odac-portal-XXXXXX"; assert(mkdtemp(dir)); struct odac c={0};
 wan_state="ONLINE";
 snprintf(c.config.database,sizeof(c.config.database),"%s/db",dir);
 c.config.enabled=1; c.config.max_users=20; c.config.max_devices=30; c.config.max_sessions=10;
 c.config.session_seconds=3600; c.config.max_audit=100; c.config.db_max_kib=2048; c.config.wan_interval=5;
 assert(!odac_open(&c));
 char csrf[65],params[512]; struct json_object *r;
 r=call(&c,"portal","csrf","{}",NULL); snprintf(csrf,sizeof(csrf),"%s",core_str(data(r),"csrf")); json_object_put(r);
 struct json_object *req=json_object_new_object(); core_addstr(req,"scope","admin"); core_addstr(req,"method","settings");
 json_object_object_add(req,"params",json_object_new_object());
 r=odac_dispatch(&c,req); json_object_put(req);
 check(core_bool(r,"ok") && core_ll(data(r),"portal_enabled")==1,"the portal starts enabled");
 json_object_put(r);
 /* A blacklisted device must stay revoked while the network is open. */
 r=call(&c,"admin","device_update","{\"mac\":\"02:00:00:00:00:10\",\"policy\":\"BLACKLIST\"}",NULL); assert(core_bool(r,"ok")); json_object_put(r);
 live_device=1; auths=0;
 req=json_object_new_object(); core_addstr(req,"scope","admin"); core_addstr(req,"method","settings_update");
 struct json_object *p=json_object_new_object(); core_addint(p,"portal_enabled",0,1);
 json_object_object_add(req,"params",p);
 r=odac_dispatch(&c,req); json_object_put(req);
 check(core_bool(r,"ok"),"the administrator can disable the portal");
 check(RT(&c)->portal_enabled==0,"the portal switch is off");
 check(auths==0,"a blacklisted device is not admitted while the portal is off");
 json_object_put(r);
 /* With the portal off no account may log in. */
 snprintf(params,sizeof(params),"{\"username\":\"anyone\",\"password\":\"password10\",\"csrf\":\"%s\"}",csrf);
 r=call(&c,"portal","login",params,NULL);
 check(!core_bool(r,"ok") && !strcmp(core_str(json_object_object_get(r,"error"),"code"),"PORTAL_DISABLED"),"login is refused while the portal is off");
 json_object_put(r);
 /* A device that is not blacklisted is admitted without any account session. */
 r=call(&c,"admin","device_update","{\"mac\":\"02:00:00:00:00:10\",\"policy\":\"NONE\"}",NULL); assert(core_bool(r,"ok")); json_object_put(r);
 auths=0; odac_test_clock+=ODAC_REFRESH_SECONDS+5; odac_tick(&c,(time_t)odac_test_clock);
 check(auths==1 && !strcmp(last_auth,"02:00:00:00:00:10"),"a device without a blacklist entry is admitted while the portal is off");
 /* The status reply tells the client that no login is needed. */
 r=call(&c,"portal","status","{}",NULL);
 check(core_bool(r,"ok") && core_bool(data(r),"portal_enabled")==0,"the portal status reports the switch as off");
 json_object_put(r);
 /* Turning it back on restores the account flow. */
 live_device=0;
 req=json_object_new_object(); core_addstr(req,"scope","admin"); core_addstr(req,"method","settings_update");
 p=json_object_new_object(); core_addint(p,"portal_enabled",1,1);
 json_object_object_add(req,"params",p);
 r=odac_dispatch(&c,req); json_object_put(req);
 check(core_bool(r,"ok") && RT(&c)->portal_enabled==1,"the administrator can enable the portal again");
 json_object_put(r);
 odac_close(&c); unlink(c.config.database); assert(!rmdir(dir));
}
/* Chinese portal copy must survive a daemon restart on json-c 0.18. */
static void portal_text_restart(void)
{
 char dir[]="/tmp/odac-text-XXXXXX"; assert(mkdtemp(dir)); struct odac c={0};
 snprintf(c.config.database,sizeof(c.config.database),"%s/db",dir);
 c.config.enabled=1; c.config.max_users=20; c.config.max_devices=30; c.config.max_sessions=10;
 c.config.session_seconds=3600; c.config.max_audit=100; c.config.db_max_kib=2048; c.config.wan_interval=5;
 assert(!odac_open(&c));
 const char *cn="{\"portal_title\":\"\xe5\xae\xbf\xe8\x88\x8d\xe7\xbd\x91\xe7\xbb\x9c\",\"portal_footer\":\"\xe6\x9c\xac\xe7\xbd\x91\xe7\xbb\x9c\xe4\xbb\x85\xe4\xbe\x9b\xe6\x8e\x88\xe6\x9d\x83\xe7\x94\xa8\xe6\x88\xb7\xe4\xbd\xbf\xe7\x94\xa8\xe3\x80\x82\"}";
 struct json_object *req=json_object_new_object(); core_addstr(req,"scope","admin"); core_addstr(req,"method","settings_update");
 json_object_object_add(req,"params",core_parse(cn,strlen(cn),16));
 struct json_object *rep=odac_dispatch(&c,req); json_object_put(req);
 check(core_bool(rep,"ok"),"settings_update accepts Chinese portal copy"); json_object_put(rep);
 odac_close(&c); assert(!odac_open(&c));
 struct json_object *st=json_object_new_object(); core_addstr(st,"scope","admin"); core_addstr(st,"method","settings");
 json_object_object_add(st,"params",json_object_new_object());
 rep=odac_dispatch(&c,st); json_object_put(st);
 check(core_bool(rep,"ok"),"settings read back after restart");
 struct json_object *title=json_object_object_get(json_object_object_get(rep,"data"),"portal_title");
 check(title && !strcmp(json_object_get_string(title),"\xe5\xae\xbf\xe8\x88\x8d\xe7\xbd\x91\xe7\xbb\x9c"),"Chinese portal title survives a restart");
 struct json_object *foot=json_object_object_get(json_object_object_get(rep,"data"),"portal_footer");
 check(foot && !strcmp(json_object_get_string(foot),"\xe6\x9c\xac\xe7\xbd\x91\xe7\xbb\x9c\xe4\xbb\x85\xe4\xbe\x9b\xe6\x8e\x88\xe6\x9d\x83\xe7\x94\xa8\xe6\x88\xb7\xe4\xbd\xbf\xe7\x94\xa8\xe3\x80\x82"),"Chinese portal footer survives a restart");
 json_object_put(rep);
 odac_close(&c); unlink(c.config.database); assert(!rmdir(dir));
}
/* An existing version-1 database must upgrade in place, keeping its rows. */
static void schema_upgrade(void)
{
 char dir[]="/tmp/odac-mig-XXXXXX"; assert(mkdtemp(dir)); char path[256];
 snprintf(path,sizeof(path),"%s/db",dir);
 sqlite3 *raw=NULL; assert(!sqlite3_open(path,&raw));
 const char *sql=
  "BEGIN IMMEDIATE;"
  "CREATE TABLE users(id INTEGER PRIMARY KEY,username TEXT NOT NULL UNIQUE,password_hash TEXT NOT NULL,status TEXT NOT NULL CHECK(status IN ('PENDING','ACTIVE','REJECTED','DISABLED','BANNED')),created_at INTEGER NOT NULL,last_login INTEGER,expires_at INTEGER,max_devices INTEGER NOT NULL DEFAULT 3,upload_kbps INTEGER,download_kbps INTEGER);"
  "CREATE TABLE registration_requests(user_id INTEGER PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE,receipt TEXT NOT NULL UNIQUE,note TEXT,mac TEXT,created_at INTEGER NOT NULL);"
  "CREATE TABLE devices(mac TEXT PRIMARY KEY,alias TEXT NOT NULL DEFAULT '',first_seen INTEGER NOT NULL);"
  "CREATE TABLE device_bindings(mac TEXT REFERENCES devices(mac) ON DELETE CASCADE,user_id INTEGER REFERENCES users(id) ON DELETE CASCADE,PRIMARY KEY(mac,user_id));"
  "CREATE TABLE device_policies(mac TEXT PRIMARY KEY REFERENCES devices(mac) ON DELETE CASCADE,policy TEXT NOT NULL DEFAULT 'NONE' CHECK(policy IN ('NONE','WHITELIST','BLACKLIST')),expires_at INTEGER,upload_kbps INTEGER,download_kbps INTEGER);"
  "CREATE TABLE audit_logs(id INTEGER PRIMARY KEY,at INTEGER NOT NULL,action TEXT NOT NULL,target TEXT NOT NULL);"
  "CREATE TABLE settings(id INTEGER PRIMARY KEY CHECK(id=1),value TEXT NOT NULL);"
  "INSERT INTO users(username,password_hash,status,created_at,max_devices) VALUES('legacy','x','ACTIVE',1,3);"
  "PRAGMA application_id=1329873219; PRAGMA user_version=1; COMMIT;";
 char *err=NULL; assert(!sqlite3_exec(raw,sql,NULL,NULL,&err)); sqlite3_close(raw);
 struct odac c={0};
 snprintf(c.config.database,sizeof(c.config.database),"%s",path);
 c.config.enabled=1; c.config.max_users=20; c.config.max_devices=30; c.config.max_sessions=10;
 c.config.session_seconds=3600; c.config.max_audit=100; c.config.db_max_kib=2048; c.config.wan_interval=5;
 check(!odac_open(&c),"a version-1 database opens after the upgrade");
 check(db_scalar(&c,"SELECT count(*) FROM users WHERE username='legacy'",0)==1,"the upgrade keeps existing accounts");
 sqlite3_stmt *s=NULL;
 assert(!sqlite3_prepare_v2(c.db,"SELECT count(*) FROM pragma_table_info('registration_requests') WHERE name IN ('ip','hostname')",-1,&s,NULL));
 check(sqlite3_step(s)==SQLITE_ROW && sqlite3_column_int(s,0)==2,"registration_requests gained the ip and hostname columns");
 sqlite3_finalize(s);
 odac_close(&c); unlink(path); assert(!rmdir(dir));
}
static void json_compat(void)
{
 /* json-c 0.18 rejects raw UTF-8 in strict mode; the compatibility layer
  * escapes it so Chinese portal copy still round-trips. */
 struct json_object *o=parse("{\"t\":\"\xe5\xae\xbf\xe8\x88\x8d\xe7\xbd\x91\xe7\xbb\x9c\"}");
 check(o && !strcmp(core_str(o,"t"),"\xe5\xae\xbf\xe8\x88\x8d\xe7\xbd\x91\xe7\xbb\x9c"),"raw UTF-8 parses to the original text");
 json_object_put(o);
 o=parse("{\"t\":\"\xf0\x9f\x98\x80\"}");
 check(o && !strcmp(core_str(o,"t"),"\xf0\x9f\x98\x80"),"astral UTF-8 round-trips through surrogate pairs");
 json_object_put(o);
 o=parse("{\"t\":\"a\\u00e9b\"}");
 check(o && !strcmp(core_str(o,"t"),"a\xc3\xa9""b"),"existing escapes are left alone");
 json_object_put(o);
 o=parse("{\"t\":\"x\ny\"}");
 check(!o,"control characters are still rejected");
 o=parse("{\"t\":1} junk");
 check(!o,"trailing garbage is still rejected");
 o=parse("{\"t\":1/*c*/}");
 check(!o,"comments are still rejected");
 o=parse("{\"t\":1,\"t\":2}");
 check(o && core_ll(o,"t")==2,"duplicate keys keep the last value");
 json_object_put(o);
 o=parse("{\"t\":\"\\ud83d\"}");
 check(!o || core_str(o,"t")[0]!=0,"a lone surrogate never crashes the parser");
 json_object_put(o);
 o=parse("{\"t\":\"\xff\xfe\"}");
 check(!o,"invalid UTF-8 is rejected");
 o=parse("{\"a\":\"\xe5\xae\xbf\",\"b\":1}");
 check(o && !strcmp(core_str(o,"a"),"\xe5\xae\xbf") && core_ll(o,"b")==1,"keys and values survive mixed content");
 json_object_put(o);
}
