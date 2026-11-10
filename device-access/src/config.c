#define _GNU_SOURCE
#include "config.h"
#include "transport.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct integer_field { const char *name; size_t offset; int minimum, maximum, initial; };
#define NUMBER(name, low, high, initial) { #name, offsetof(struct odac_config, name), low, high, initial }
static const struct integer_field integers[] = {
    NUMBER(enabled, 0, 1, 0), NUMBER(portal_port, 1024, 65535, 2080),
    NUMBER(portal_https_port, 1024, 65535, 8443),
    NUMBER(session_seconds, 60, 604800, 3600), NUMBER(max_users, 1, 2000, 200),
    NUMBER(max_devices, 1, 5000, 500), NUMBER(max_sessions, 1, 256, 64),
    NUMBER(upload_kbps, 0, 1000000, 10000), NUMBER(download_kbps, 0, 1000000, 30000),
    NUMBER(wan_interval, 5, 30, 15), NUMBER(max_audit, 10, 10000, 500),
    NUMBER(db_max_kib, 512, 65536, 2048)
};
struct string_field { const char *name; size_t offset, capacity; const char *initial; };
#define TEXT(name, initial) { #name, offsetof(struct odac_config, name), sizeof(((struct odac_config *)0)->name), initial }
static const struct string_field strings[] = {
    TEXT(database, "/etc/odac/odac.db"), TEXT(socket_path, ODAC_SOCKET),
    TEXT(interface, ""), TEXT(gateway, ""), TEXT(upstream, ""),
    TEXT(status_file, "/var/run/odac/upstream.status"), TEXT(pid_file, ""),
    TEXT(portal_hostname, ""), TEXT(tls_cert, ""), TEXT(tls_key, "")
};

static int hostname_valid(const char *name)
{
    size_t length = strlen(name);
    if (!length || length > 253 || name[0] == '.' || name[length - 1] == '.') return 0;
    size_t label = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c == '.') {
            if (!label || name[i - 1] == '-') return 0;
            label = 0;
        } else {
            if (!(isalnum(c) || c == '-') || (!label && c == '-') || ++label > 63) return 0;
        }
    }
    return label > 0 && name[length - 1] != '-';
}

/* Space-separated IPv4 list (the form `uci get` prints for a list option). */
static int upstream_valid(const char *list)
{
    char copy[272], *save = NULL;
    struct in_addr ip;
    int count = 0;
    if (strlen(list) >= sizeof(copy)) return 0;
    strcpy(copy, list);
    for (char *token = strtok_r(copy, " ", &save); token; token = strtok_r(NULL, " ", &save))
        if (++count > ODAC_UPSTREAM_MAX || inet_pton(AF_INET, token, &ip) != 1) return 0;
    return 1;
}

static int valid_configuration(const struct odac_config *config)
{
    struct in_addr ip;
    if (config->database[0] != '/' || config->socket_path[0] != '/' ||
        strlen(config->socket_path) > 100 || config->portal_port == config->portal_https_port ||
        !upstream_valid(config->upstream)) return 0;
    if (!config->enabled) return 1;
    if (inet_pton(AF_INET, config->gateway, &ip) != 1 ||
        !hostname_valid(config->portal_hostname) || config->tls_cert[0] != '/' ||
        config->tls_key[0] != '/' || !config->interface[0]) return 0;
    for (const char *p = config->interface; *p; p++)
        if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '-' || *p == '.')) return 0;
    return strcmp(config->interface, "lo") && strcmp(config->interface, "br-lan") &&
           strcmp(config->interface, "lan") && strcmp(config->interface, "wan");
}

int odac_config_load(struct odac_config *config, const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    struct stat info;
    if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_uid != geteuid() ||
        (info.st_mode & 0022) || info.st_size <= 0 || info.st_size > 16384) {
        close(fd); return -1;
    }
    char buffer[16385];
    ssize_t count = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);
    if (count != info.st_size) return -1;
    struct json_object *document = odac_json_parse(buffer, (size_t)count, ODAC_MESSAGE_MAX);
    if (!document) return -1;
    memset(config, 0, sizeof(*config));
    int valid = 1;
    for (size_t i = 0; i < sizeof(integers) / sizeof(integers[0]); i++) {
        const struct integer_field *field = &integers[i];
        int *destination = (int *)((char *)config + field->offset);
        *destination = field->initial;
        struct json_object *value;
        if (!json_object_object_get_ex(document, field->name, &value)) continue;
        if (!json_object_is_type(value, json_type_int)) { valid = 0; break; }
        int64_t number = json_object_get_int64(value);
        if (number < field->minimum || number > field->maximum) { valid = 0; break; }
        *destination = (int)number;
    }
    for (size_t i = 0; valid && i < sizeof(strings) / sizeof(strings[0]); i++) {
        const struct string_field *field = &strings[i];
        const char *source = field->initial;
        struct json_object *value;
        if (json_object_object_get_ex(document, field->name, &value)) {
            if (!json_object_is_type(value, json_type_string)) { valid = 0; break; }
            source = json_object_get_string(value);
            if (strlen(source) != (size_t)json_object_get_string_len(value)) { valid = 0; break; }
        }
        if (strlen(source) >= field->capacity || strpbrk(source, "\r\n\t")) { valid = 0; break; }
        memcpy((char *)config + field->offset, source, strlen(source) + 1);
    }
    json_object_object_foreach(document, key, value) {
        (void)value;
        int known = 0;
        for (size_t i = 0; i < sizeof(strings) / sizeof(strings[0]); i++) known |= !strcmp(key, strings[i].name);
        for (size_t i = 0; i < sizeof(integers) / sizeof(integers[0]); i++) known |= !strcmp(key, integers[i].name);
        if (!known) valid = 0;
    }
    json_object_put(document);
    return valid && valid_configuration(config) ? 0 : -1;
}
