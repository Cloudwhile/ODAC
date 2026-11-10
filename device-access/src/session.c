#include "core.h"
#include <stdio.h>
#include <string.h>

int grant_has(struct odac *c,const char *mac)
{
 for(int i=0;i<ODAC_GRANTS;i++) if(!strcmp(RT(c)->granted[i],mac)) return 1;
 return 0;
}
int grant_count(struct odac *c)
{
 int n=0; for(int i=0;i<ODAC_GRANTS;i++) n+=!!RT(c)->granted[i][0]; return n;
}
int core_pending(struct odac *c)
{
 if(RT(c)->overflow) return 1;
 for(int i=0;i<ODAC_PENDING;i++) if(RT(c)->pending[i][0]) return 1;
 return 0;
}
int core_deauth(struct odac *c,const char *mac,int force)
{
 if(!force && !grant_has(c,mac)) return 0;
 if(odac_platform_deauth(c,mac)) {
  int slot=-1;
  for(int i=0;i<ODAC_PENDING;i++) {
   if(!strcmp(RT(c)->pending[i],mac)) { slot=i; break; }
   if(slot<0 && !RT(c)->pending[i][0]) slot=i;
  }
  if(slot>=0) snprintf(RT(c)->pending[slot],18,"%s",mac); else RT(c)->overflow=1;
  core_close_gate(c); return -1;
 }
 for(int i=0;i<ODAC_GRANTS;i++) if(!strcmp(RT(c)->granted[i],mac)) RT(c)->granted[i][0]=0;
 for(int i=0;i<ODAC_PENDING;i++) if(!strcmp(RT(c)->pending[i],mac)) RT(c)->pending[i][0]=0;
 return 0;
}
int grant_apply(struct odac *c,const char *mac,int seconds,int up,int down)
{
 if(!RT(c)->online || core_pending(c) || seconds<1) return -1;
 int slot=-1;
 for(int i=0;i<ODAC_GRANTS;i++) {
  if(!strcmp(RT(c)->granted[i],mac)) { slot=i; break; }
  if(slot<0 && !RT(c)->granted[i][0]) slot=i;
 }
 if(slot<0 || odac_platform_auth(c,mac,seconds,up,down)) { core_close_gate(c); core_deauth(c,mac,1); return -1; }
 snprintf(RT(c)->granted[slot],18,"%s",mac); return 0;
}
/* Open mode admits every device that is not blacklisted. A device that cannot
 * be granted is left out on its own; the gate stays open for the others. */
int grant_apply_soft(struct odac *c,const char *mac,int seconds,int up,int down)
{
 if(!RT(c)->online || core_pending(c) || seconds<1) return -1;
 int slot=-1;
 for(int i=0;i<ODAC_GRANTS;i++) {
  if(!strcmp(RT(c)->granted[i],mac)) { slot=i; break; }
  if(slot<0 && !RT(c)->granted[i][0]) slot=i;
 }
 if(slot<0) return -1;
 if(odac_platform_auth(c,mac,seconds,up,down)) { core_deauth(c,mac,1); return -1; }
 snprintf(RT(c)->granted[slot],18,"%s",mac); return 0;
}
void session_retry(struct odac *c)
{
 for(int i=0;i<ODAC_PENDING;i++) if(RT(c)->pending[i][0]) {
  char mac[18]; snprintf(mac,sizeof(mac),"%s",RT(c)->pending[i]); core_deauth(c,mac,1);
 }
}
struct core_session *session_by_mac(struct odac *c,const char *mac)
{
 for(int i=0;i<ODAC_SESSIONS;i++) if(RT(c)->sessions[i].mac[0] && !strcmp(RT(c)->sessions[i].mac,mac)) return &RT(c)->sessions[i];
 return NULL;
}
struct core_session *session_find(struct odac *c,const char *token,const char *mac)
{
 struct core_session *s=session_by_mac(c,mac);
 return s && core_hex64(token) && core_equal(s->token,token) && session_valid(c,s)?s:NULL;
}
void session_drop(struct core_session *s) { explicit_bzero(s,sizeof(*s)); }
int session_count(struct odac *c,int user)
{
 int n=0;
 for(int i=0;i<ODAC_SESSIONS;i++) if(RT(c)->sessions[i].mac[0] && (!user || RT(c)->sessions[i].user_id==user)) n++;
 return n;
}
int session_valid(struct odac *c,struct core_session *s)
{
 if(!s || !s->mac[0] || s->expires<=RT(c)->now) return 0;
 time_t expires=0; int kind=policy_kind(c,s->mac,&expires);
 if(kind<0 || kind==POLICY_BLACKLIST || kind==POLICY_EXPIRED) return 0;
 sqlite3_stmt *q=db_prepare(c,"SELECT status,expires_at FROM users WHERE id=?"); int valid=0;
 if(q) {
  sqlite3_bind_int(q,1,s->user_id);
  int rc=sqlite3_step(q);
  if(rc!=SQLITE_ROW && rc!=SQLITE_DONE) core_close_gate(c);
  if(rc==SQLITE_ROW) valid=!strcmp((const char *)sqlite3_column_text(q,0),"ACTIVE") && (sqlite3_column_type(q,1)==SQLITE_NULL || sqlite3_column_int64(q,1)>RT(c)->now);
 } else core_close_gate(c);
 sqlite3_finalize(q); return valid;
}
int session_apply(struct odac *c,struct core_session *s)
{
 int up,down;
 if(!session_valid(c,s)) { s->authorized=0; return core_deauth(c,s->mac,1)<0?ENFORCE_REVOKE_FAILED:ENFORCE_IDLE; }
 if(!RT(c)->online) { s->authorized=0; return core_deauth(c,s->mac,0)<0?ENFORCE_REVOKE_FAILED:ENFORCE_IDLE; }
 if(policy_rates(c,s->mac,s->user_id,&up,&down)) { core_close_gate(c); return ENFORCE_AUTH_FAILED; }
 s->authorized=grant_apply(c,s->mac,(int)(s->expires-RT(c)->now),up,down)==0;
 return s->authorized?ENFORCE_GRANTED:ENFORCE_AUTH_FAILED;
}
int session_revoke_user(struct odac *c,int user)
{
 int rc=0;
 for(int i=0;i<ODAC_SESSIONS;i++) {
  struct core_session *s=&RT(c)->sessions[i];
  if(!s->mac[0] || (user && s->user_id!=user)) continue;
  if(core_deauth(c,s->mac,1)) rc=-1;
  session_drop(s);
 }
 return rc;
}
int session_revoke_all(struct odac *c)
{
 int rc=session_revoke_user(c,0);
 for(int i=0;i<ODAC_GRANTS;i++) if(RT(c)->granted[i][0]) {
  char mac[18]; snprintf(mac,sizeof(mac),"%s",RT(c)->granted[i]); if(core_deauth(c,mac,1)) rc=-1;
 }
 return rc;
}
int session_reconcile(struct odac *c,int user,int maximum)
{
 int rc=0;
 for(int i=ODAC_SESSIONS-1;i>=0 && session_count(c,user)>maximum;i--) {
  struct core_session *s=&RT(c)->sessions[i];
  if(!s->mac[0] || (user && s->user_id!=user)) continue;
  if(core_deauth(c,s->mac,1)) rc=-1;
  session_drop(s);
 }
 return rc;
}
void session_expire(struct odac *c)
{
 for(int i=0;i<ODAC_SESSIONS;i++) {
  struct core_session *s=&RT(c)->sessions[i];
  if(s->mac[0] && !session_valid(c,s)) { core_deauth(c,s->mac,1); session_drop(s); }
 }
}
struct json_object *session_create(struct odac *c,int user,int maximum,time_t expires,const char *mac)
{
 struct core_session *old=session_by_mac(c,mac), *s=NULL;
 int own=old && old->user_id==user;
 if(session_count(c,user)-own>=maximum || session_count(c,0)-(old!=NULL)>=c->config.max_sessions) return core_error("SESSION_LIMIT","Session capacity reached");
 if(old) { if(core_deauth(c,mac,1)) return core_error("ENFORCEMENT_PENDING","Revocation failed"); session_drop(old); s=old; }
 for(int i=0;!s && i<ODAC_SESSIONS;i++) if(!RT(c)->sessions[i].mac[0]) s=&RT(c)->sessions[i];
 if(!s) return core_error("SESSION_LIMIT","Session capacity reached");
 if(core_random(s->token)) return core_error("CRYPTO_FAILED","Randomness unavailable");
 snprintf(s->mac,sizeof(s->mac),"%s",mac); s->user_id=user; s->expires=RT(c)->now+c->config.session_seconds;
 if(expires && expires<s->expires) s->expires=expires;
 if(session_apply(c,s)<0) { core_deauth(c,mac,1); session_drop(s); return core_error("ENFORCEMENT_FAILED","Authorization failed"); }
 struct json_object *o=json_object_new_object(); core_addstr(o,"token",s->token); core_addint(o,"expires_at",s->expires,1); return core_ok(o);
}
