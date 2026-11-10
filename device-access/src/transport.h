#ifndef ODAC_TRANSPORT_H
#define ODAC_TRANSPORT_H
#include <json-c/json.h>
#include <stddef.h>
/* Requests stay small; replies may carry one page of a list (ubus caps a
 * message at 1 MiB, so keep replies well below it). */
#define ODAC_MESSAGE_MAX 65536
#define ODAC_REPLY_MAX 524288
#define ODAC_SOCKET "/var/run/odac/control.sock"
struct json_object *odac_json_parse(const char *text, size_t length, size_t limit);
struct json_object *odac_receive(int fd, size_t limit, int64_t deadline);
int odac_send(int fd, struct json_object *value, int64_t deadline);
struct json_object *odac_exchange_deadline(const char *path, struct json_object *request, int64_t budget_ms);
struct json_object *odac_exchange(const char *path, struct json_object *request);
struct json_object *odac_failure(const char *code, const char *message);
#endif
