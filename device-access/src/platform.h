/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef ODAC_PLATFORM_INTERNAL_H
#define ODAC_PLATFORM_INTERNAL_H
#include "odac.h"
#include <stddef.h>
#define ODAC_PLATFORM_LEASE 90
#define ODAC_PLATFORM_OUTPUT (256 * 1024)
int odac_platform_valid_interface(const char *value);
int odac_platform_valid_mac(const char *value);
int odac_platform_valid_ipv4(const char *value);
int odac_platform_run(const char *const argv[], char *output, size_t capacity, int timeout_ms);
struct json_object *odac_platform_parse_json(const char *text);
struct json_object *odac_platform_parse_client(struct json_object *root, const char *ip, const char *mac);
int odac_platform_neighbor_matches(const char *text, const char *interface, const char *ip, const char *mac);
int odac_platform_neighbor_ip(const char *text, const char *interface, const char *mac, char ip[16]);
const char *odac_platform_string(struct json_object *object, const char *key);
int odac_platform_status_file(const char *path, time_t now, int max_age, char state[20]);
int odac_platform_wan_proven(struct odac *ctx);
long long odac_platform_wan_remaining(void);
#endif
