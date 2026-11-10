#define _GNU_SOURCE
#include "config.h"
#include "transport.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CONFIG_PATH "/var/run/odac/config.json"
#define COOKIE_NAME "odac_session"
#ifdef ODAC_TEST_BUILD
#define CONFIG_SOURCE (getenv("ODAC_TEST_CONFIG") ? getenv("ODAC_TEST_CONFIG") : CONFIG_PATH)
#else
#define CONFIG_SOURCE CONFIG_PATH
#endif
static const char *const actions[] = { "register", "login", "logout", "status" };

static const char *environment(const char *name)
{
    const char *value = getenv(name);
    return value ? value : "";
}

static void respond(int status, const char *cookie, struct json_object *body)
{
    const char *reason = status == 200 ? "OK" : status == 429 ? "Too Many Requests" :
                         status == 503 ? "Service Unavailable" : status == 405 ? "Method Not Allowed" : "Bad Request";
    printf("Status: %d %s\r\nContent-Type: application/json; charset=utf-8\r\n"
           "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
           "Referrer-Policy: no-referrer\r\nX-Frame-Options: DENY\r\n", status, reason);
    if (cookie) printf("Set-Cookie: %s\r\n", cookie);
    printf("\r\n%s\n", json_object_to_json_string_ext(body, JSON_C_TO_STRING_PLAIN));
}

static void fail(int status, const char *code, const char *message)
{
    struct json_object *body = odac_failure(code, message);
    respond(status, NULL, body);
    json_object_put(body);
}

static const char *query_action(void)
{
    const char *query = environment("QUERY_STRING");
    if (strncmp(query, "action=", 7)) return NULL;
    const char *value = query + 7;
    for (size_t i = 0; i < sizeof(actions) / sizeof(actions[0]); i++)
        if (!strcmp(value, actions[i])) return actions[i];
    return NULL;
}

static int cookie_token(char *token, size_t capacity)
{
    const char *cookie = environment("HTTP_COOKIE");
    const char *name = COOKIE_NAME "=";
    for (const char *p = cookie; *p;) {
        while (*p == ' ' || *p == ';') p++;
        if (!strncmp(p, name, strlen(name))) {
            p += strlen(name);
            size_t length = strcspn(p, ";");
            if (!length || length >= capacity) return 0;
            for (size_t i = 0; i < length; i++)
                if (!isalnum((unsigned char)p[i]) && p[i] != '-' && p[i] != '_') return 0;
            memcpy(token, p, length);
            token[length] = '\0';
            return 1;
        }
        p += strcspn(p, ";");
    }
    return 0;
}

static struct json_object *read_body(void)
{
    const char *length_text = environment("CONTENT_LENGTH");
    char *end;
    long length = strtol(length_text, &end, 10);
    if (!*length_text || *end || length <= 0 || length > 8192) return NULL;
    char *buffer = malloc((size_t)length + 1);
    if (!buffer) return NULL;
    size_t used = 0;
    while (used < (size_t)length) {
        size_t count = fread(buffer + used, 1, (size_t)length - used, stdin);
        if (!count) break;
        used += count;
    }
    struct json_object *body = used == (size_t)length ? odac_json_parse(buffer, used, ODAC_MESSAGE_MAX) : NULL;
    explicit_bzero(buffer, (size_t)length);
    free(buffer);
    return body;
}

static void landing(const struct odac_config *config)
{
    printf("Status: 302 Found\r\nLocation: https://%s:%d/\r\nCache-Control: no-store\r\n"
           "Content-Type: text/plain; charset=utf-8\r\n\r\n请通过安全门户继续登录：https://%s:%d/\n",
           config->portal_hostname, config->portal_https_port,
           config->portal_hostname, config->portal_https_port);
}

static int status_for(struct json_object *reply)
{
    struct json_object *ok, *error, *code;
    if (json_object_object_get_ex(reply, "ok", &ok) && json_object_get_boolean(ok)) return 200;
    if (json_object_object_get_ex(reply, "error", &error) &&
        json_object_object_get_ex(error, "code", &code)) {
        const char *name = json_object_get_string(code);
        if (!strcmp(name, "RATE_LIMITED")) return 429;
        if (!strcmp(name, "WAN_UNAVAILABLE") || !strcmp(name, "UNAVAILABLE")) return 503;
    }
    return 400;
}

static char *session_cookie(struct json_object *reply, const char *action, int *owned)
{
    static char expired[] = COOKIE_NAME "=; Path=/; Max-Age=0; Secure; HttpOnly; SameSite=Strict";
    struct json_object *ok, *data, *token;
    *owned = 0;
    if (!json_object_object_get_ex(reply, "ok", &ok) || !json_object_get_boolean(ok)) return NULL;
    if (!strcmp(action, "logout")) return expired;
    if (strcmp(action, "login") || !json_object_object_get_ex(reply, "data", &data) ||
        !json_object_object_get_ex(data, "token", &token)) return NULL;
    char *cookie = NULL;
    if (asprintf(&cookie, COOKIE_NAME "=%s; Path=/; Secure; HttpOnly; SameSite=Strict",
                 json_object_get_string(token)) < 0) return NULL;
    json_object_object_del(data, "token");
    *owned = 1;
    return cookie;
}

int main(int argc, char **argv)
{
    (void)argc;
    struct odac_config config;
    if (odac_config_load(&config, CONFIG_SOURCE) || !config.enabled) {
        fail(503, "UNAVAILABLE", "认证服务未启用");
        return 0;
    }
    const char *program = strrchr(argv[0], '/');
    if (!strcmp(program ? program + 1 : argv[0], "landing")) { landing(&config); return 0; }
    if (strcmp(environment("HTTPS"), "on")) { fail(400, "HTTPS_REQUIRED", "请使用 HTTPS 门户"); return 0; }
    const char *action = query_action();
    const char *method = environment("REQUEST_METHOD");
    struct in_addr address;
    if (!action) { fail(400, "BAD_REQUEST", "未知操作"); return 0; }
    if (inet_pton(AF_INET, environment("REMOTE_ADDR"), &address) != 1) {
        fail(400, "UNSUPPORTED_CLIENT", "当前仅支持 IPv4 认证网络"); return 0;
    }
    struct json_object *params;
    if (!strcmp(method, "POST")) {
        if (strncmp(environment("CONTENT_TYPE"), "application/json", 16)) {
            fail(400, "BAD_REQUEST", "请求格式无效"); return 0;
        }
        params = read_body();
        if (!params) { fail(400, "BAD_REQUEST", "请求内容无效"); return 0; }
    } else if (!strcmp(method, "GET") && !strcmp(action, "status")) {
        params = json_object_new_object();
    } else { fail(405, "METHOD_NOT_ALLOWED", "请求方法无效"); return 0; }
    json_object_object_del(params, "token");
    char token[160];
    int have_token = cookie_token(token, sizeof(token));
    struct json_object *request = json_object_new_object();
    json_object_object_add(request, "scope", json_object_new_string("portal"));
    json_object_object_add(request, "method", json_object_new_string(action));
    json_object_object_add(request, "peer", json_object_new_string(environment("REMOTE_ADDR")));
    json_object_object_add(request, "http_method", json_object_new_string(method));
    json_object_object_add(request, "origin", json_object_new_string(environment("HTTP_ORIGIN")));
    json_object_object_add(request, "host", json_object_new_string(environment("HTTP_HOST")));
    json_object_object_add(request, "content_type", json_object_new_string(environment("CONTENT_TYPE")));
    json_object_object_add(request, "https", json_object_new_boolean(1));
    json_object_object_add(request, "params", params);
    if (have_token) json_object_object_add(request, "token", json_object_new_string(token));
    explicit_bzero(token, sizeof(token));
    struct json_object *reply = odac_exchange(config.socket_path, request);
    json_object_put(request);
    if (!reply) { fail(503, "UNAVAILABLE", "认证服务暂不可用"); return 0; }
    int owned;
    char *cookie = session_cookie(reply, action, &owned);
    respond(status_for(reply), cookie, reply);
    if (owned) { explicit_bzero(cookie, strlen(cookie)); free(cookie); }
    json_object_put(reply);
    return 0;
}
