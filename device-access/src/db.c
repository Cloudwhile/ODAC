#define _GNU_SOURCE
#include "core.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

sqlite3_stmt *db_prepare(struct odac *c, const char *sql)
{
 sqlite3_stmt *s = NULL;
 if (sqlite3_prepare_v2(c->db, sql, -1, &s, NULL) != SQLITE_OK) { core_close_gate(c); return NULL; }
 return s;
}
int db_exec(struct odac *c, const char *sql)
{
 if(sqlite3_exec(c->db,sql,NULL,NULL,NULL)==SQLITE_OK) return 0;
 core_close_gate(c); return -1;
}
long long db_scalar(struct odac *c, const char *sql, long long arg)
{
 sqlite3_stmt *s=db_prepare(c,sql); long long n=-1;
 if(s) { sqlite3_bind_int64(s,1,arg); if(sqlite3_step(s)==SQLITE_ROW) n=sqlite3_column_int64(s,0); }
 sqlite3_finalize(s); if(n<0) core_close_gate(c); return n;
}
struct json_object *db_collect(sqlite3_stmt *s)
{
 if(!s) return NULL;
 struct json_object *a=json_object_new_array(); int rc;
 while((rc=sqlite3_step(s))==SQLITE_ROW) {
  struct json_object *o=json_object_new_object();
  for(int i=0;i<sqlite3_column_count(s);i++) {
   struct json_object *v=NULL;
   if(sqlite3_column_type(s,i)==SQLITE_INTEGER) v=json_object_new_int64(sqlite3_column_int64(s,i));
   else if(sqlite3_column_type(s,i)!=SQLITE_NULL) v=json_object_new_string((const char *)sqlite3_column_text(s,i));
   json_object_object_add(o,sqlite3_column_name(s,i),v);
  }
  json_object_array_add(a,o);
 }
 sqlite3_finalize(s);
 if(rc!=SQLITE_DONE) { json_object_put(a); return NULL; }
 return a;
}
void db_audit(struct odac *c,const char *actor,const char *action,const char *target,const char *detail)
{
 (void)actor; (void)detail;
 sqlite3_stmt *s=db_prepare(c,"INSERT INTO audit_logs(at,action,target) VALUES(?,?,?)");
 if(s) { sqlite3_bind_int64(s,1,RT(c)->now); sqlite3_bind_text(s,2,action,-1,SQLITE_TRANSIENT); sqlite3_bind_text(s,3,target,-1,SQLITE_TRANSIENT); if(sqlite3_step(s)!=SQLITE_DONE) core_close_gate(c); }
 else core_close_gate(c);
 sqlite3_finalize(s);
}
void db_wan(struct odac *c,const char *state) { db_audit(c,"system","wan",state,""); }
void db_cleanup(struct odac *c)
{
 sqlite3_stmt *s=db_prepare(c,"DELETE FROM audit_logs WHERE at<? OR id NOT IN(SELECT id FROM audit_logs ORDER BY id DESC LIMIT ?)");
 if(s) { sqlite3_bind_int64(s,1,RT(c)->now-ODAC_RETENTION_SECONDS); sqlite3_bind_int(s,2,c->config.max_audit); if(sqlite3_step(s)!=SQLITE_DONE) core_close_gate(c); } else core_close_gate(c);
 sqlite3_finalize(s);
}
int db_init(struct odac *c)
{
 struct stat st;
 int fd=open(c->config.database,O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
 if (fd<0) return -1;
 if (fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_uid!=geteuid() || st.st_nlink!=1 || fchmod(fd,0600)) { close(fd); return -1; }
 close(fd);
 if (sqlite3_open_v2(c->config.database,&c->db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_NOMUTEX,NULL)!=SQLITE_OK) return -1;
 sqlite3_busy_timeout(c->db,1500);
 sqlite3_limit(c->db,SQLITE_LIMIT_LENGTH,65536);
 sqlite3_limit(c->db,SQLITE_LIMIT_SQL_LENGTH,16384);
 if (db_exec(c,"PRAGMA foreign_keys=ON; PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL; PRAGMA secure_delete=ON; PRAGMA trusted_schema=OFF; PRAGMA temp_store=MEMORY; PRAGMA cache_size=-256;")) return -1;
 char pragma[128];
 sqlite3_stmt *s=db_prepare(c,"PRAGMA page_size");
 int pagesize=4096;
 if (s && sqlite3_step(s)==SQLITE_ROW) pagesize=sqlite3_column_int(s,0);
 sqlite3_finalize(s);
 snprintf(pragma,sizeof(pragma),"PRAGMA max_page_count=%d",c->config.db_max_kib*1024/pagesize);
 if (db_exec(c,pragma)) return -1;
 s=db_prepare(c,"PRAGMA user_version");
 int version=-1;
 if (s && sqlite3_step(s)==SQLITE_ROW) version=sqlite3_column_int(s,0);
 sqlite3_finalize(s);
 if (version<0 || version>3) return -1;
 if (!version && db_exec(c,
 "BEGIN IMMEDIATE;"
 "CREATE TABLE users(id INTEGER PRIMARY KEY,username TEXT NOT NULL UNIQUE,password_hash TEXT NOT NULL,status TEXT NOT NULL CHECK(status IN ('PENDING','ACTIVE','REJECTED','DISABLED','BANNED')),created_at INTEGER NOT NULL,last_login INTEGER,expires_at INTEGER,max_devices INTEGER NOT NULL DEFAULT 3,upload_kbps INTEGER,download_kbps INTEGER);"
 "CREATE TABLE registration_requests(user_id INTEGER PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE,receipt TEXT NOT NULL UNIQUE,note TEXT,mac TEXT,ip TEXT,hostname TEXT,created_at INTEGER NOT NULL);"
 "CREATE TABLE devices(mac TEXT PRIMARY KEY,alias TEXT NOT NULL DEFAULT '',first_seen INTEGER NOT NULL);"
 "CREATE TABLE device_bindings(mac TEXT REFERENCES devices(mac) ON DELETE CASCADE,user_id INTEGER REFERENCES users(id) ON DELETE CASCADE,PRIMARY KEY(mac,user_id));"
 "CREATE TABLE device_policies(mac TEXT PRIMARY KEY REFERENCES devices(mac) ON DELETE CASCADE,policy TEXT NOT NULL DEFAULT 'NONE' CHECK(policy IN ('NONE','WHITELIST','BLACKLIST')),expires_at INTEGER,upload_kbps INTEGER,download_kbps INTEGER);"
 "CREATE TABLE audit_logs(id INTEGER PRIMARY KEY,at INTEGER NOT NULL,actor TEXT NOT NULL DEFAULT 'system',action TEXT NOT NULL,target TEXT NOT NULL,detail TEXT NOT NULL DEFAULT '');"
 "CREATE TABLE settings(id INTEGER PRIMARY KEY CHECK(id=1),value TEXT NOT NULL);"
 "PRAGMA application_id=1329873219; PRAGMA user_version=3; COMMIT;")) { db_exec(c,"ROLLBACK"); return -1; }
 if (version==1 && db_exec(c,"BEGIN IMMEDIATE; ALTER TABLE audit_logs ADD COLUMN actor TEXT NOT NULL DEFAULT 'system'; ALTER TABLE audit_logs ADD COLUMN detail TEXT NOT NULL DEFAULT ''; ALTER TABLE registration_requests ADD COLUMN ip TEXT; ALTER TABLE registration_requests ADD COLUMN hostname TEXT; PRAGMA user_version=3; COMMIT;")) { db_exec(c,"ROLLBACK"); return -1; }
 if (version==2 && db_exec(c,"BEGIN IMMEDIATE; ALTER TABLE registration_requests ADD COLUMN ip TEXT; ALTER TABLE registration_requests ADD COLUMN hostname TEXT; PRAGMA user_version=3; COMMIT;")) { db_exec(c,"ROLLBACK"); return -1; }
 s=db_prepare(c,"PRAGMA quick_check");
 int valid=s && sqlite3_step(s)==SQLITE_ROW && !strcmp((const char *)sqlite3_column_text(s,0),"ok");
 sqlite3_finalize(s);
 return valid ? 0 : -1;
}
