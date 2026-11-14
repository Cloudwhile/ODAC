#include "json_text.h"
#include <ctype.h>
#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>

static const char hex[] = "0123456789abcdef";

static size_t append_escape(char *out, size_t used, unsigned int code)
{
    out[used++] = '\\';
    out[used++] = 'u';
    out[used++] = hex[(code >> 12) & 0xf];
    out[used++] = hex[(code >> 8) & 0xf];
    out[used++] = hex[(code >> 4) & 0xf];
    out[used++] = hex[code & 0xf];
    return used;
}

char *odac_json_escape_utf8(const char *text, size_t length, size_t *out_length)
{
    if (!text || !length || !out_length) return NULL;
    /* Every byte expands to at most six characters. */
    char *out = malloc(length * 6 + 1);
    if (!out) return NULL;
    size_t used = 0, i = 0;
    int in_string = 0, escaped = 0;
    while (i < length) {
        unsigned char c = (unsigned char)text[i];
        if (!in_string) {
            out[used++] = (char)c;
            if (c == '"') in_string = 1;
            i++;
            continue;
        }
        if (escaped) { out[used++] = (char)c; escaped = 0; i++; continue; }
        if (c == '\\') { out[used++] = (char)c; escaped = 1; i++; continue; }
        if (c == '"') { out[used++] = (char)c; in_string = 0; i++; continue; }
        if (c < 0x80) { out[used++] = (char)c; i++; continue; }
        size_t need;
        unsigned int code;
        if ((c & 0xe0) == 0xc0) { need = 1; code = c & 0x1fu; }
        else if ((c & 0xf0) == 0xe0) { need = 2; code = c & 0x0fu; }
        else if ((c & 0xf8) == 0xf0) { need = 3; code = c & 0x07u; }
        else { out[used++] = (char)c; i++; continue; }
        if (i + need >= length) { out[used++] = (char)c; i++; continue; }
        int valid = 1;
        for (size_t k = 1; k <= need; k++) {
            unsigned char next = (unsigned char)text[i + k];
            if ((next & 0xc0) != 0x80) { valid = 0; break; }
            code = (code << 6) | (next & 0x3fu);
        }
        /* Overlong forms, surrogates and out-of-range code points stay raw so
         * the strict parse rejects them instead of silently accepting them. */
        if (!valid || code < 0x80 || (code >= 0xd800 && code <= 0xdfff) || code > 0x10ffff ||
            (need == 1 && code < 0x80) || (need == 2 && code < 0x800) || (need == 3 && code < 0x10000)) {
            out[used++] = (char)c;
            i++;
            continue;
        }
        if (code >= 0x10000) {
            unsigned int offset = code - 0x10000;
            used = append_escape(out, used, 0xd800 + (offset >> 10));
            used = append_escape(out, used, 0xdc00 + (offset & 0x3ff));
        } else used = append_escape(out, used, code);
        i += need + 1;
    }
    out[used] = '\0';
    *out_length = used;
    return out;
}

struct json_object *odac_json_parse_strict(const char *text, size_t length, int depth)
{
    if (!text || !length || length > 0x7fffffff || memchr(text, 0, length)) return NULL;
    /* RFC 8259 forbids unescaped control characters inside strings. json-c
     * 0.17 accepts them even in strict mode, so reject them here instead of
     * depending on the library version. */
    int has_high = 0, in_string = 0, after_backslash = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c >= 0x80) has_high = 1;
        if (!in_string) { if (c == '"') in_string = 1; continue; }
        if (after_backslash) { after_backslash = 0; continue; }
        if (c == '\\') after_backslash = 1;
        else if (c == '"') in_string = 0;
        else if (c < 0x20) return NULL;
    }
    const char *source = text;
    size_t source_length = length;
    char *escaped = NULL;
    if (has_high) {
        escaped = odac_json_escape_utf8(text, length, &source_length);
        if (!escaped) return NULL;
        /* Every valid sequence is now a \u escape; any remaining high byte is
         * invalid UTF-8, which json-c 0.17 would otherwise accept. */
        for (size_t i = 0; i < source_length; i++)
            if ((unsigned char)escaped[i] >= 0x80) { free(escaped); return NULL; }
        source = escaped;
    }
    struct json_tokener *t = json_tokener_new_ex(depth);
    if (!t) { free(escaped); return NULL; }
    json_tokener_set_flags(t, JSON_TOKENER_STRICT);
    struct json_object *v = json_tokener_parse_ex(t, source, (int)source_length);
    size_t end = json_tokener_get_parse_end(t);
    int ok = json_tokener_get_error(t) == json_tokener_success;
    while (end < source_length && isspace((unsigned char)source[end])) end++;
    json_tokener_free(t);
    free(escaped);
    if (!ok || end != source_length || !json_object_is_type(v, json_type_object)) {
        if (v) json_object_put(v);
        return NULL;
    }
    return v;
}
