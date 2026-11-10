#define _GNU_SOURCE
#include "transport.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct json_object *odac_wait_ready(const char *path, struct json_object *request);

static int usage(void)
{
    fprintf(stderr, "usage: odacctl [--socket PATH] METHOD|--wait-ready < params.json\n");
    return 2;
}

static struct json_object *read_params(void)
{
    char *buffer = malloc(ODAC_MESSAGE_MAX + 1);
    if (!buffer) return NULL;
    size_t used = fread(buffer, 1, ODAC_MESSAGE_MAX + 1, stdin);
    struct json_object *params = NULL;
    size_t start = 0;
    while (start < used && isspace((unsigned char)buffer[start])) start++;
    if (used <= ODAC_MESSAGE_MAX) {
        if (start == used) params = json_object_new_object();
        else params = odac_json_parse(buffer + start, used - start, ODAC_MESSAGE_MAX);
    }
    explicit_bzero(buffer, used);
    free(buffer);
    return params;
}

int main(int argc, char **argv)
{
    const char *path = ODAC_SOCKET;
    int index = 1;
    if (argc == 4 && !strcmp(argv[1], "--socket")) { path = argv[2]; index = 3; }
    else if (argc != 2) return usage();
    int wait_ready = !strcmp(argv[index], "--wait-ready");
    const char *method = wait_ready ? "overview" : argv[index];
    for (const char *p = method; *p; p++)
        if (!islower((unsigned char)*p) && *p != '_') return usage();
    struct json_object *params = read_params();
    struct json_object *reply;
    if (!params) reply = odac_failure("BAD_REQUEST", "params must be one JSON object");
    else {
        struct json_object *request = json_object_new_object();
        json_object_object_add(request, "scope", json_object_new_string("admin"));
        json_object_object_add(request, "method", json_object_new_string(method));
        json_object_object_add(request, "params", params);
        reply = wait_ready ? odac_wait_ready(path, request) : odac_exchange(path, request);
        json_object_put(request);
        if (!reply) reply = odac_failure("UNAVAILABLE", "odacd is not reachable");
    }
    puts(json_object_to_json_string_ext(reply, JSON_C_TO_STRING_PLAIN));
    struct json_object *ok;
    int success = json_object_object_get_ex(reply, "ok", &ok) && json_object_get_boolean(ok);
    json_object_put(reply);
    return success ? 0 : 1;
}
