// SPDX-License-Identifier: GPL-3.0-only
// Strata core: the little JSON it needs (messages in from the page, messages out). No allocation tricks: a parsed
// message is one tree freed with js_free; output goes into a growing buffer (jb_*).
#ifndef STRATA_JSON_H
#define STRATA_JSON_H
#include <stddef.h>
#include <stdint.h>
enum { JS_NULL, JS_FALSE, JS_TRUE, JS_NUM, JS_STR, JS_ARR, JS_OBJ };
typedef struct js js_t;
struct js {
    int type;
    double num;
    char *str;                 /* JS_STR: the text; in an object: also each member's key (key) */
    char *key;
    js_t *kid, *next;          /* JS_ARR / JS_OBJ: the first member, then next */
    int n;                     /* members */
};
js_t *js_parse(const char *s);                /* NULL: not JSON */
void js_free(js_t *j);
js_t *js_get(const js_t *o, const char *key); /* NULL when absent (or o is not an object) */
js_t *js_at(const js_t *a, int i);
int js_has(const js_t *o, const char *key);   /* present and not null */
double js_num(const js_t *o, const char *key, double def);
int js_truthy(const js_t *j);                 /* JavaScript's !!value */
const char *js_strv(const js_t *o, const char *key);   /* NULL when absent / not a string */
char *js_dump(const js_t *j);                 /* a tree back to JSON text (malloc'd; free it) */

typedef struct { char *b; size_t n, cap; } jb_t;
void jb_init(jb_t *b);
void jb_free(jb_t *b);
void jb_raw(jb_t *b, const char *s);
void jb_rawn(jb_t *b, const char *s, size_t n);
void jb_ch(jb_t *b, char c);
void jb_str(jb_t *b, const char *s);          /* a quoted, escaped string (bytes >= 0x80 as Latin-1 characters, as the firmwares' text) */
void jb_str_utf8(jb_t *b, const char *s);     /* the same for UTF-8 text: bytes >= 0x80 pass through */
void jb_num(jb_t *b, double v);               /* as JSON.stringify prints it (shortest round trip; NaN / Inf: null) */
void jb_int(jb_t *b, long long v);
void jb_bool(jb_t *b, int v);
void jb_key(jb_t *b, const char *k);          /* "k": (with a comma before it when needed) */
void jb_comma(jb_t *b);                       /* a comma unless right after [ { or : */
void jb_b64(jb_t *b, const uint8_t *p, size_t n);   /* a quoted base64 string */
size_t b64_decode(const char *s, uint8_t *out, size_t max);
#endif
