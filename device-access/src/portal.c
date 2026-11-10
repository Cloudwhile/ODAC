#include "core.h"
#include <stdio.h>
#include <string.h>

struct json_object *portal_dispatch(struct odac *c,struct json_object *r,struct json_object *p)
{
 const char *ip=core_str(r,"peer"), *method=core_str(r,"method");
 if(!core_ipv4(ip)) return core_error("PEER","Verified IPv4 peer required");
 struct core_peer *peer=NULL,*oldest=&RT(c)->peers[0];
 for(int i=0;i<ODAC_PEERS;i++) {
  struct core_peer *x=&RT(c)->peers[i];
  if(!strcmp(x->ip,ip)) { peer=x; break; }
  if(x->seen<oldest->seen) oldest=x;
 }
 if(!peer) {
  /* Do not evict live counters to let rotating peers reset the global limit. */
  if(oldest->ip[0] && RT(c)->now-oldest->seen<ODAC_CSRF_SECONDS) return core_error("RATE_LIMIT","Peer capacity reached");
  peer=oldest; memset(peer,0,sizeof(*peer)); snprintf(peer->ip,sizeof(peer->ip),"%s",ip);
 }
 peer->seen=RT(c)->now;
 if(!peer->window || RT(c)->now<peer->window || RT(c)->now-peer->window>=ODAC_PEER_WINDOW) { peer->window=RT(c)->now; peer->hits=0; }
 if(++peer->hits>ODAC_PEER_REQUESTS) return core_error("RATE_LIMIT","Request limit reached");
 if(!strcmp(method,"csrf")) {
  if(core_random(peer->csrf)) return core_error("CRYPTO_FAILED","Randomness unavailable");
  peer->csrf_expires=RT(c)->now+ODAC_CSRF_SECONDS;
  struct json_object *o=json_object_new_object(); core_addstr(o,"csrf",peer->csrf); core_addint(o,"expires_at",peer->csrf_expires,1); return core_ok(o);
 }
 struct json_object *client=odac_platform_client(c,ip); char mac[18];
 if(!client || core_mac_normalize(core_str(client,"mac"),mac)) { json_object_put(client); return core_error("PEER","Neighbor verification failed"); }
 json_object_put(client);
 /* Tokens are accepted only from the trusted request envelope, never params. */
 struct core_session *s=session_find(c,core_str(r,"token"),mac);
 if(!strcmp(method,"status")) {
  if(peer->csrf_expires<=RT(c)->now) {
   if(core_random(peer->csrf)) return core_error("CRYPTO_FAILED","Randomness unavailable");
   peer->csrf_expires=RT(c)->now+ODAC_CSRF_SECONDS;
  }
  struct json_object *o=json_object_new_object(), *device=json_object_new_object(), *session=json_object_new_object();
  core_addstr(o,"csrf",peer->csrf); core_addstr(o,"mac",mac);
  json_object_object_add(o,"text",core_portal_texts(c));
  time_t expiry=0; core_addstr(device,"mac",mac); core_addstr(device,"ip",ip); core_addstr(device,"policy",policy_name(policy_kind(c,mac,&expiry)));
  json_object_object_add(o,"device",device); json_object_object_add(session,"active",json_object_new_boolean(s!=NULL));
  if(s) {
   core_addint(session,"expires_at",s->expires,1); sqlite3_stmt *q=db_prepare(c,"SELECT username FROM users WHERE id=?");
   if(q) { sqlite3_bind_int(q,1,s->user_id); if(sqlite3_step(q)==SQLITE_ROW) core_addstr(session,"username",(const char *)sqlite3_column_text(q,0)); } sqlite3_finalize(q);
  }
  json_object_object_add(o,"session",session);
  json_object_object_add(o,"authenticated",json_object_new_boolean(s!=NULL));
  json_object_object_add(o,"portal_enabled",json_object_new_boolean(RT(c)->portal_enabled));
  json_object_object_add(o,"granted",json_object_new_boolean(s && s->authorized && RT(c)->online)); return core_ok(o);
 }
 if(strcmp(method,"register") && strcmp(method,"login") && strcmp(method,"logout")) return core_error("METHOD","Unknown portal method");
 if(!core_bool(r,"https")) return core_error("HTTPS_REQUIRED","HTTPS required");
 if(peer->csrf_expires<=RT(c)->now || !core_equal(peer->csrf,core_str(p,"csrf"))) return core_error("CSRF","Invalid or expired CSRF token");
 if(!strcmp(method,"logout")) {
  if(!s) return core_error("SESSION","Valid session required");
  session_drop(s); return core_enforced(core_deauth(c,mac,1),json_object_new_object());
 }
 /* With the portal disabled the network is open: no account may log in. */
 if(!RT(c)->portal_enabled) return core_error("PORTAL_DISABLED","The portal is disabled");
 if(!peer->auth_window || RT(c)->now<peer->auth_window || RT(c)->now-peer->auth_window>=ODAC_AUTH_WINDOW) { peer->auth_window=RT(c)->now; peer->auth_hits=0; }
 if(!RT(c)->auth_window || RT(c)->now<RT(c)->auth_window || RT(c)->now-RT(c)->auth_window>=ODAC_GLOBAL_WINDOW) { RT(c)->auth_window=RT(c)->now; RT(c)->auth_hits=0; }
 if(++peer->auth_hits>ODAC_PEER_AUTH || ++RT(c)->auth_hits>ODAC_GLOBAL_AUTH) return core_error("RATE_LIMIT","Authentication limit reached");
 if(!strcmp(method,"register")) return account_register(c,p,mac,ip);
 return account_login(c,p,mac);
}
