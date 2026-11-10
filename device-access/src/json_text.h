#ifndef ODAC_JSON_TEXT_H
#define ODAC_JSON_TEXT_H
#include <stddef.h>
/* json-c 0.18 (the version OpenWrt 25.12 ships) compares string bytes with a
 * signed char in its strict mode, so any UTF-8 byte >= 0x80 is mistaken for a
 * control character and the whole document is rejected. Upstream fixed this in
 * 0.19 with an explicit unsigned cast.
 *
 * Rewriting raw UTF-8 as \uXXXX escapes before the strict parse keeps the
 * strict guarantees (no control characters, no comments, no trailing garbage)
 * while accepting Chinese portal copy. The rewrite only touches bytes inside
 * JSON string literals and never changes the decoded value.
 *
 * Returns a malloc'd buffer, or NULL on failure. The caller frees it. */
char *odac_json_escape_utf8(const char *text, size_t length, size_t *out_length);
/* Strict JSON object parse that tolerates raw UTF-8 on json-c 0.18. */
struct json_object *odac_json_parse_strict(const char *text, size_t length, int depth);
#endif
