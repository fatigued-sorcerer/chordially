// SPDX-License-Identifier: GPL-3.0-only
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "json.h"

static const char *ws(const char *s) { while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++; return s; }
static js_t *mk(int t) { js_t *j = calloc(1, sizeof *j); j->type = t; return j; }
static void put_utf8(char **o, unsigned c)
{
    char *p = *o;
    if (c < 0x80) *p++ = (char)c;
    else if (c < 0x800) { *p++ = (char)(0xC0 | c >> 6); *p++ = (char)(0x80 | (c & 63)); }
    else if (c < 0x10000) { *p++ = (char)(0xE0 | c >> 12); *p++ = (char)(0x80 | (c >> 6 & 63)); *p++ = (char)(0x80 | (c & 63)); }
    else { *p++ = (char)(0xF0 | c >> 18); *p++ = (char)(0x80 | (c >> 12 & 63)); *p++ = (char)(0x80 | (c >> 6 & 63)); *p++ = (char)(0x80 | (c & 63)); }
    *o = p;
}
static char *pstr(const char **ps)
{
    const char *s = *ps + 1, *e = s;
    while (*e && *e != '"') { if (*e == '\\' && e[1]) e++; e++; }
    if (*e != '"') return NULL;
    char *out = malloc((size_t)(e - s) * 2 + 1), *o = out;
    while (s < e) {
        if (*s != '\\') { *o++ = *s++; continue; }
        s++;
        switch (*s) {
        case 'n': *o++ = '\n'; s++; break; case 't': *o++ = '\t'; s++; break; case 'r': *o++ = '\r'; s++; break;
        case 'b': *o++ = '\b'; s++; break; case 'f': *o++ = '\f'; s++; break;
        case 'u': { unsigned c = (unsigned)strtoul((char[5]){ s[1], s[2], s[3], s[4], 0 }, NULL, 16); s += 5;
            if (c >= 0xD800 && c < 0xDC00 && s[0] == '\\' && s[1] == 'u') {
                unsigned d = (unsigned)strtoul((char[5]){ s[2], s[3], s[4], s[5], 0 }, NULL, 16);
                if (d >= 0xDC00 && d < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00); s += 6; }
            }
            put_utf8(&o, c); break; }
        default: *o++ = *s++; break;
        }
    }
    *o = 0;
    *ps = e + 1;
    return out;
}
static js_t *pval(const char **ps, int depth)
{
    const char *s = ws(*ps);
    js_t *j = NULL;
    if (depth > 64) return NULL;
    if (*s == '{' || *s == '[') {
        int obj = *s == '{';
        j = mk(obj ? JS_OBJ : JS_ARR);
        js_t **tail = &j->kid;
        s = ws(s + 1);
        if (*s == (obj ? '}' : ']')) { *ps = s + 1; return j; }
        for (;;) {
            char *key = NULL;
            s = ws(s);
            if (obj) {
                if (*s != '"' || !(key = pstr(&s))) { js_free(j); return NULL; }
                s = ws(s);
                if (*s != ':') { free(key); js_free(j); return NULL; }
                s++;
            }
            js_t *v = pval(&s, depth + 1);
            if (!v) { free(key); js_free(j); return NULL; }
            v->key = key; *tail = v; tail = &v->next; j->n++;
            s = ws(s);
            if (*s == ',') { s++; continue; }
            if (*s == (obj ? '}' : ']')) { s++; break; }
            js_free(j); return NULL;
        }
    } else if (*s == '"') {
        j = mk(JS_STR);
        if (!(j->str = pstr(&s))) { free(j); return NULL; }
    } else if (!strncmp(s, "true", 4)) { j = mk(JS_TRUE); s += 4; }
    else if (!strncmp(s, "false", 5)) { j = mk(JS_FALSE); s += 5; }
    else if (!strncmp(s, "null", 4)) { j = mk(JS_NULL); s += 4; }
    else {
        char *e;
        double v = strtod(s, &e);
        if (e == s) return NULL;
        j = mk(JS_NUM); j->num = v; s = e;
    }
    *ps = s;
    return j;
}
js_t *js_parse(const char *s) { return s ? pval(&s, 0) : NULL; }
void js_free(js_t *j)
{
    while (j) { js_t *n = j->next; js_free(j->kid); free(j->str); free(j->key); free(j); j = n; }
}
js_t *js_get(const js_t *o, const char *key)
{
    if (!o || o->type != JS_OBJ) return NULL;
    js_t *f = NULL;
    for (js_t *k = o->kid; k; k = k->next) if (!strcmp(k->key, key)) f = k;   /* (the last one wins, as in JS) */
    return f;
}
js_t *js_at(const js_t *a, int i) { if (!a || a->type != JS_ARR) return NULL; js_t *k = a->kid; while (k && i--) k = k->next; return k; }
int js_has(const js_t *o, const char *key) { js_t *j = js_get(o, key); return j && j->type != JS_NULL; }
double js_num(const js_t *o, const char *key, double def)
{
    js_t *j = js_get(o, key);
    if (!j) return def;
    if (j->type == JS_NUM) return j->num;
    if (j->type == JS_TRUE) return 1;
    if (j->type == JS_FALSE || j->type == JS_NULL) return 0;
    if (j->type == JS_STR) return strtod(j->str, NULL);
    return def;
}
int js_truthy(const js_t *j)
{
    if (!j) return 0;
    switch (j->type) {
    case JS_NULL: case JS_FALSE: return 0;
    case JS_NUM: return j->num != 0 && !isnan(j->num);
    case JS_STR: return j->str[0] != 0;
    default: return 1;
    }
}
const char *js_strv(const js_t *o, const char *key) { js_t *j = js_get(o, key); return j && j->type == JS_STR ? j->str : NULL; }

/* ---- output ---- */
void jb_init(jb_t *b) { b->cap = 256; b->n = 0; b->b = malloc(b->cap); b->b[0] = 0; }
void jb_free(jb_t *b) { free(b->b); b->b = NULL; b->n = b->cap = 0; }
static void grow(jb_t *b, size_t add) { if (b->n + add + 1 > b->cap) { while (b->n + add + 1 > b->cap) b->cap *= 2; b->b = realloc(b->b, b->cap); } }
void jb_rawn(jb_t *b, const char *s, size_t n) { grow(b, n); memcpy(b->b + b->n, s, n); b->n += n; b->b[b->n] = 0; }
void jb_raw(jb_t *b, const char *s) { jb_rawn(b, s, strlen(s)); }
void jb_ch(jb_t *b, char c) { grow(b, 1); b->b[b->n++] = c; b->b[b->n] = 0; }
void jb_comma(jb_t *b) { if (b->n) { char c = b->b[b->n - 1]; if (c != '[' && c != '{' && c != ':') jb_ch(b, ','); } }
void jb_str(jb_t *b, const char *s)
{
    jb_ch(b, '"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned c = *p;
        if (c == '"' || c == '\\') { jb_ch(b, '\\'); jb_ch(b, (char)c); }
        else if (c == '\n') jb_raw(b, "\\n");
        else if (c == '\r') jb_raw(b, "\\r");
        else if (c == '\t') jb_raw(b, "\\t");
        else if (c == '\b') jb_raw(b, "\\b");
        else if (c == '\f') jb_raw(b, "\\f");
        else if (c < 0x20) { char t[8]; snprintf(t, sizeof t, "\\u%04x", c); jb_raw(b, t); }
        else if (c >= 0x80) {          /* the firmwares' text is ASCII; a stray byte is one character, as String.fromCharCode makes it */
            char t[3] = { (char)(0xC0 | c >> 6), (char)(0x80 | (c & 63)), 0 };
            jb_raw(b, t);
        } else jb_ch(b, (char)c);
    }
    jb_ch(b, '"');
}
void jb_str_utf8(jb_t *b, const char *s)
{
    jb_ch(b, '"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned c = *p;
        if (c == '"' || c == '\\') { jb_ch(b, '\\'); jb_ch(b, (char)c); }
        else if (c == '\n') jb_raw(b, "\\n");
        else if (c == '\r') jb_raw(b, "\\r");
        else if (c == '\t') jb_raw(b, "\\t");
        else if (c < 0x20) { char t[8]; snprintf(t, sizeof t, "\\u%04x", c); jb_raw(b, t); }
        else jb_ch(b, (char)c);
    }
    jb_ch(b, '"');
}
void jb_num(jb_t *b, double v)
{
    char t[40];
    if (isnan(v) || isinf(v)) { jb_raw(b, "null"); return; }
    if (v == 0) { jb_raw(b, "0"); return; }
    if (v == floor(v) && fabs(v) < 1e21) { snprintf(t, sizeof t, "%.0f", v); jb_raw(b, t); return; }
    for (int p = 1; p <= 17; p++) { snprintf(t, sizeof t, "%.*g", p, v); if (strtod(t, NULL) == v) break; }
    jb_raw(b, t);
}
void jb_int(jb_t *b, long long v) { char t[24]; snprintf(t, sizeof t, "%lld", v); jb_raw(b, t); }
void jb_bool(jb_t *b, int v) { jb_raw(b, v ? "true" : "false"); }
void jb_key(jb_t *b, const char *k) { jb_comma(b); jb_str(b, k); jb_ch(b, ':'); }
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
void jb_b64(jb_t *b, const uint8_t *p, size_t n)
{
    grow(b, (n + 2) / 3 * 4 + 2);
    char *o = b->b + b->n;
    *o++ = '"';
    size_t i = 0;
    for (; i + 2 < n; i += 3) { unsigned v = p[i] << 16 | p[i + 1] << 8 | p[i + 2]; *o++ = B64[v >> 18]; *o++ = B64[v >> 12 & 63]; *o++ = B64[v >> 6 & 63]; *o++ = B64[v & 63]; }
    if (i < n) {
        unsigned v = p[i] << 16 | (i + 1 < n ? p[i + 1] << 8 : 0);
        *o++ = B64[v >> 18]; *o++ = B64[v >> 12 & 63]; *o++ = i + 1 < n ? B64[v >> 6 & 63] : '='; *o++ = '=';
    }
    *o++ = '"';
    b->n = (size_t)(o - b->b); b->b[b->n] = 0;
}
size_t b64_decode(const char *s, uint8_t *out, size_t max)
{
    unsigned v = 0; int bits = 0; size_t n = 0;
    for (; *s && *s != '='; s++) {
        const char *q = strchr(B64, *s);
        if (!q || !*s) continue;
        v = v << 6 | (unsigned)(q - B64); bits += 6;
        if (bits >= 8) { bits -= 8; if (n < max) out[n] = (uint8_t)(v >> bits); n++; }
    }
    return n;
}

/* a tree back to JSON text (malloc'd; free it): engine commands and states pass through the core as text */
static void js_dump_to(jb_t *b, const js_t *j)
{
    switch (j->type) {
    case JS_NULL: jb_raw(b, "null"); break;
    case JS_FALSE: jb_raw(b, "false"); break;
    case JS_TRUE: jb_raw(b, "true"); break;
    case JS_NUM: jb_num(b, j->num); break;
    case JS_STR: jb_str_utf8(b, j->str ? j->str : ""); break;
    case JS_ARR: jb_ch(b, '['); for (const js_t *k = j->kid; k; k = k->next) { jb_comma(b); js_dump_to(b, k); } jb_ch(b, ']'); break;
    case JS_OBJ: jb_ch(b, '{'); for (const js_t *k = j->kid; k; k = k->next) { jb_key(b, k->key ? k->key : ""); js_dump_to(b, k); } jb_ch(b, '}'); break;
    }
}
char *js_dump(const js_t *j)
{
    jb_t b; jb_init(&b); if (j) js_dump_to(&b, j); jb_ch(&b, 0);
    return b.b;                                /* (jb's buffer, given away) */
}
