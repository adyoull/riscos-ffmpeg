/*
 * sources.c - what Reel plays: files, and web addresses given in text
 * (see sources.h). Plain C, no RISC OS calls: tests/host/sources_test.c
 * runs it on the host. Part of riscos-ffmpeg (Reel). GPL v2 or later.
 */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "sources.h"

#define TEXT_MAX (16 << 20)     /* the most read of a text file (yt-dlp -J can be big) */

/* ---- small helpers ------------------------------------------------------ */

static char *dup_n(const char *s, size_t n)
{
    char *d = malloc(n + 1);
    if (d) {
        memcpy(d, s, n);
        d[n] = 0;
    }
    return d;
}

static char *dup_s(const char *s) { return s ? dup_n(s, strlen(s)) : NULL; }

void source_free(source_t *s)
{
    free(s->url); free(s->audio_url); free(s->headers); free(s->user_agent);
    free(s->title); free(s->key);
    memset(s, 0, sizeof(*s));
}

int source_simple(source_t *s, const char *url)
{
    memset(s, 0, sizeof(*s));
    return (s->url = dup_s(url)) ? 0 : -1;
}

int source_copy(source_t *d, const source_t *s)
{
    memset(d, 0, sizeof(*d));
    d->url = dup_s(s->url);
    d->audio_url = dup_s(s->audio_url);
    d->headers = dup_s(s->headers);
    d->user_agent = dup_s(s->user_agent);
    d->title = dup_s(s->title);
    d->key = dup_s(s->key);
    if (!d->url) {
        source_free(d);
        return -1;
    }
    return 0;
}

/* "scheme://..." with a scheme of letters, digits, + - . */
static int is_address(const char *s, size_t n)
{
    size_t i = 0;
    while (i < n && (isalnum((unsigned char)s[i]) || s[i] == '+' || s[i] == '-' || s[i] == '.'))
        i++;
    return i > 1 && i + 3 <= n && s[i] == ':' && s[i + 1] == '/' && s[i + 2] == '/' && isalpha((unsigned char)s[0]);
}

void utf8_to_latin1(char *s)
{
    unsigned char *r = (unsigned char *)s, *w = (unsigned char *)s;
    while (*r) {
        unsigned c = *r, cp;
        int more;
        if (c < 0x80) { *w++ = *r++; continue; }
        if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; more = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; more = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; more = 3; }
        else { *w++ = '?'; r++; continue; }           /* not UTF-8 after all */
        r++;
        while (more-- && (*r & 0xC0) == 0x80)
            cp = (cp << 6) | (*r++ & 0x3F);
        if (cp < 0x100)
            *w++ = (unsigned char)cp;
        else if (cp == 0x2018 || cp == 0x2019 || cp == 0x2032)
            *w++ = '\'';
        else if (cp == 0x201C || cp == 0x201D)
            *w++ = '"';
        else if (cp == 0x2013 || cp == 0x2014 || cp == 0x2212)
            *w++ = '-';
        else if (cp == 0x2026) {
            *w++ = '.'; *w++ = '.'; if (w <= r) *w++ = '.';
        } else
            *w++ = '?';
    }
    *w = 0;
}

void source_name(const source_t *s, char *buf, size_t size)
{
    const char *u = s->url ? s->url : "";
    if (s->title && *s->title) {
        snprintf(buf, size, "%s", s->title);
        return;
    }
    if (is_address(u, strlen(u))) {
        const char *host = strstr(u, "://") + 3, *path = strchr(host, '/'), *end, *last;
        size_t hl = path ? (size_t)(path - host) : strlen(host);
        end = path ? path + strcspn(path, "?#") : NULL;
        last = NULL;
        if (end) {                                  /* the last non-empty part of the path */
            const char *p = end;
            while (p > path && p[-1] == '/')
                p--;
            last = p;
            while (last > path && last[-1] != '/')
                last--;
            if (last < p)
                snprintf(buf, size, "%.*s: %.*s", (int)hl, host, (int)(p - last), last);
            else
                snprintf(buf, size, "%.*s", (int)hl, host);
        } else
            snprintf(buf, size, "%.*s", (int)hl, host);
        return;
    }
    {
        const char *p = strrchr(u, u[0] == '/' ? '/' : '.');   /* RISC OS or Unix name */
        snprintf(buf, size, "%s", p ? p + 1 : u);
    }
}

void source_key(const source_t *s, char *buf, size_t size)
{
    const char *k = s->key ? s->key : s->url ? s->url : "";
    if (strlen(k) < size) {
        snprintf(buf, size, "%s", k);
        return;
    }
    {                                               /* FNV-1a of a long address */
        uint64_t h = 1469598103934665603ULL;
        for (const unsigned char *p = (const unsigned char *)k; *p; p++)
            h = (h ^ *p) * 1099511628211ULL;
        snprintf(buf, size, "%.*s#%016llx", (int)(size > 40 ? size - 18 : 0), k, (unsigned long long)h);
    }
}

/* ---- JSON (enough for yt-dlp's) ---------------------------------------- */

enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };

typedef struct jv {
    int type;
    char *s;                    /* J_STR (UTF-8) */
    int count;
    struct jv **item;           /* J_ARR, J_OBJ */
    char **key;                 /* J_OBJ */
} jv;

typedef struct { const char *p, *end; int depth; } jparser;

static void jfree(jv *v)
{
    if (!v)
        return;
    for (int i = 0; i < v->count; i++) {
        jfree(v->item[i]);
        if (v->key)
            free(v->key[i]);
    }
    free(v->item);
    free(v->key);
    free(v->s);
    free(v);
}

static void jspace(jparser *j)
{
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r'))
        j->p++;
}

static void put_utf8(char **w, unsigned cp)
{
    unsigned char *o = (unsigned char *)*w;
    if (cp < 0x80) *o++ = (unsigned char)cp;
    else if (cp < 0x800) { *o++ = 0xC0 | (cp >> 6); *o++ = 0x80 | (cp & 0x3F); }
    else if (cp < 0x10000) { *o++ = 0xE0 | (cp >> 12); *o++ = 0x80 | ((cp >> 6) & 0x3F); *o++ = 0x80 | (cp & 0x3F); }
    else { *o++ = 0xF0 | (cp >> 18); *o++ = 0x80 | ((cp >> 12) & 0x3F); *o++ = 0x80 | ((cp >> 6) & 0x3F);
           *o++ = 0x80 | (cp & 0x3F); }
    *w = (char *)o;
}

static int hex4(const char *p, unsigned *v)
{
    *v = 0;
    for (int i = 0; i < 4; i++) {
        int c = p[i], d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                          c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0)
            return -1;
        *v = *v * 16 + (unsigned)d;
    }
    return 0;
}

/* a string at j->p (after the opening quote): a new UTF-8 string */
static char *jstring(jparser *j)
{
    const char *start = j->p;
    char *out, *w;
    size_t n = 0;
    while (j->p < j->end && *j->p != '"') {       /* its length (escapes only shrink) */
        if (*j->p == '\\')
            j->p++;
        j->p++;
        n++;
    }
    if (j->p >= j->end)
        return NULL;
    j->p = start;
    if (!(out = w = malloc(n * 4 + 1)))
        return NULL;
    while (*j->p != '"') {
        char c = *j->p++;
        if (c != '\\') {
            *w++ = c;
            continue;
        }
        c = *j->p++;
        switch (c) {
        case 'n': *w++ = '\n'; break;
        case 't': *w++ = '\t'; break;
        case 'r': *w++ = '\r'; break;
        case 'b': *w++ = '\b'; break;
        case 'f': *w++ = '\f'; break;
        case 'u': {
            unsigned cp, lo;
            if (j->end - j->p < 4 || hex4(j->p, &cp) < 0) { free(out); return NULL; }
            j->p += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && j->end - j->p >= 6 && j->p[0] == '\\' && j->p[1] == 'u' &&
                hex4(j->p + 2, &lo) == 0 && lo >= 0xDC00 && lo < 0xE000) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                j->p += 6;
            }
            put_utf8(&w, cp);
            break;
        }
        default: *w++ = c;                      /* \" \\ \/ */
        }
    }
    j->p++;                                     /* the closing quote */
    *w = 0;
    return out;
}

static jv *jvalue(jparser *j);

static int jadd(jv *v, char *key, jv *item)
{
    jv **ni = realloc(v->item, (v->count + 1) * sizeof(*ni));
    if (!ni)
        return -1;
    v->item = ni;
    if (v->type == J_OBJ) {
        char **nk = realloc(v->key, (v->count + 1) * sizeof(*nk));
        if (!nk)
            return -1;
        v->key = nk;
        v->key[v->count] = key;
    }
    v->item[v->count++] = item;
    return 0;
}

static jv *jvalue(jparser *j)
{
    jv *v;
    jspace(j);
    if (j->p >= j->end || ++j->depth > 64)
        return NULL;
    if (!(v = calloc(1, sizeof(*v))))
        return NULL;
    switch (*j->p) {
    case '"':
        j->p++;
        v->type = J_STR;
        if (!(v->s = jstring(j)))
            goto bad;
        break;
    case '{': case '[': {
        int obj = *j->p == '{';
        char close = obj ? '}' : ']';
        v->type = obj ? J_OBJ : J_ARR;
        j->p++;
        jspace(j);
        if (j->p < j->end && *j->p == close) {
            j->p++;
            break;
        }
        for (;;) {
            char *key = NULL;
            jv *item;
            if (obj) {
                jspace(j);
                if (j->p >= j->end || *j->p != '"')
                    goto bad;
                j->p++;
                if (!(key = jstring(j)))
                    goto bad;
                jspace(j);
                if (j->p >= j->end || *j->p != ':') { free(key); goto bad; }
                j->p++;
            }
            if (!(item = jvalue(j)) || jadd(v, key, item) < 0) {
                free(key);
                jfree(item);
                goto bad;
            }
            jspace(j);
            if (j->p < j->end && *j->p == ',') { j->p++; continue; }
            if (j->p < j->end && *j->p == close) { j->p++; break; }
            goto bad;
        }
        break;
    }
    case 't': case 'f': case 'n':
        v->type = *j->p == 'n' ? J_NULL : J_BOOL;
        while (j->p < j->end && isalpha((unsigned char)*j->p))
            j->p++;
        break;
    default:
        v->type = J_NUM;
        while (j->p < j->end && strchr("+-0123456789.eE", *j->p))
            j->p++;
        break;
    }
    j->depth--;
    return v;
bad:
    jfree(v);
    return NULL;
}

static jv *jget(const jv *o, const char *key)
{
    if (!o || o->type != J_OBJ)
        return NULL;
    for (int i = 0; i < o->count; i++)
        if (!strcmp(o->key[i], key))
            return o->item[i];
    return NULL;
}

static const char *jstr(const jv *o, const char *key)
{
    const jv *v = jget(o, key);
    return v && v->type == J_STR ? v->s : NULL;
}

/* yt-dlp's http_headers: User-Agent apart, the rest as header lines */
static void take_headers(source_t *s, const jv *h)
{
    size_t n = 0;
    if (!h || h->type != J_OBJ)
        return;
    for (int i = 0; i < h->count; i++)
        if (h->item[i]->type == J_STR)
            n += strlen(h->key[i]) + strlen(h->item[i]->s) + 4;
    free(s->headers);
    s->headers = NULL;
    if (!(s->headers = calloc(1, n + 1)))
        return;
    for (int i = 0; i < h->count; i++) {
        if (h->item[i]->type != J_STR)
            continue;
        if (!strcasecmp(h->key[i], "User-Agent")) {
            free(s->user_agent);
            s->user_agent = dup_s(h->item[i]->s);
        } else {
            strcat(s->headers, h->key[i]);
            strcat(s->headers, ": ");
            strcat(s->headers, h->item[i]->s);
            strcat(s->headers, "\r\n");
        }
    }
    if (!*s->headers) {
        free(s->headers);
        s->headers = NULL;
    }
}

static int is_none(const char *codec) { return codec && !strcmp(codec, "none"); }

/* One of yt-dlp's video objects (or a playlist of them) into out */
static int from_ytdlp(const jv *o, source_t *out, int max)
{
    const jv *entries = jget(o, "entries"), *rf = jget(o, "requested_formats");
    const jv *vf = NULL, *af = NULL;
    const char *type = jstr(o, "_type");
    source_t *s;
    int n = 0;

    if (entries && entries->type == J_ARR) {         /* -J of a playlist */
        for (int i = 0; i < entries->count && n < max; i++)
            n += from_ytdlp(entries->item[i], out + n, max - n);
        return n;
    }
    if (max < 1 || (type && (!strcmp(type, "url") || !strcmp(type, "url_transparent"))))
        return 0;                                    /* (a web page, not something to play) */
    if (rf && rf->type == J_ARR) {
        for (int i = 0; i < rf->count; i++) {
            const jv *f = rf->item[i];
            if (!jstr(f, "url"))
                continue;
            if (!vf && !is_none(jstr(f, "vcodec")))
                vf = f;
            else if (!af && is_none(jstr(f, "vcodec")) && !is_none(jstr(f, "acodec")))
                af = f;
        }
    }
    s = out;
    memset(s, 0, sizeof(*s));
    if (vf) {
        s->url = dup_s(jstr(vf, "url"));
        if (af)
            s->audio_url = dup_s(jstr(af, "url"));
        take_headers(s, jget(vf, "http_headers"));
    } else if (jstr(o, "url"))
        s->url = dup_s(jstr(o, "url"));
    if (!s->url) {
        source_free(s);
        return 0;
    }
    if (!s->headers && !s->user_agent)
        take_headers(s, jget(o, "http_headers"));
    if (jstr(o, "title")) {
        s->title = dup_s(jstr(o, "title"));
        if (s->title)
            utf8_to_latin1(s->title);
    }
    s->key = dup_s(jstr(o, "webpage_url") ? jstr(o, "webpage_url") : jstr(o, "original_url"));
    return 1;
}

/* JSON: one object, or several (yt-dlp -j of a playlist: one a line) */
static int parse_json(const char *text, size_t len, source_t *out, int max)
{
    jparser j = { text, text + len, 0 };
    int n = 0;
    for (;;) {
        jv *v;
        jspace(&j);
        if (j.p >= j.end || n >= max)
            break;
        if (!(v = jvalue(&j)))
            break;
        if (v->type == J_OBJ)
            n += from_ytdlp(v, out + n, max - n);
        else if (v->type == J_ARR)
            for (int i = 0; i < v->count && n < max; i++)
                n += from_ytdlp(v->item[i], out + n, max - n);
        jfree(v);
    }
    return n;
}

/* ---- text ----------------------------------------------------------------- */

int sources_parse(const char *text, size_t len, source_t *out, int max, int *hls)
{
    const char *p = text, *end = text + len;
    char *title = NULL;
    int n = 0, uri_file = 0;

    *hls = 0;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || (unsigned char)*p == 0xEF ||
                       (unsigned char)*p == 0xBB || (unsigned char)*p == 0xBF))
        p++;                                         /* (and a UTF-8 byte order mark) */
    if (p < end && (*p == '{' || *p == '['))
        return parse_json(p, end - p, out, max);
    if (end - p >= 4 && !memcmp(p, "URI", 3) && (p[3] == ' ' || p[3] == '\t'))
        uri_file = 1;                                /* Acorn URI file: "URI <tab> 100" */

    while (p < end && n < max) {
        const char *e = memchr(p, '\n', end - p), *a, *b;
        if (!e)
            e = end;
        a = p;
        b = e;
        p = e < end ? e + 1 : end;
        while (a < b && (*a == ' ' || *a == '\t'))
            a++;
        while (b > a && (b[-1] == '\r' || b[-1] == ' ' || b[-1] == '\t'))
            b--;
        if (a == b)
            continue;
        if (uri_file) {                              /* its header line */
            uri_file = 0;
            if (!memcmp(a, "URI", 3))
                continue;
        }
        if (*a == '#') {
            if (b - a > 7 && !memcmp(a, "#EXT-X-", 7)) {
                *hls = 1;                            /* an HLS playlist: FFmpeg plays it */
                for (int i = 0; i < n; i++)
                    source_free(&out[i]);
                free(title);
                return 0;
            }
            if (b - a > 8 && !memcmp(a, "#EXTINF:", 8)) {
                const char *c = memchr(a, ',', b - a);
                free(title);
                title = c && c + 1 < b ? dup_n(c + 1, b - c - 1) : NULL;
            }
            continue;
        }
        if (!is_address(a, b - a))
            continue;
        memset(&out[n], 0, sizeof(out[n]));
        if (!(out[n].url = dup_n(a, b - a)))
            break;
        if (title) {
            utf8_to_latin1(title);
            out[n].title = title;
            title = NULL;
        }
        n++;
    }
    free(title);
    /* yt-dlp -g of "bestvideo+bestaudio": the video's address, then the sound's */
    if (n == 2 && !out[0].title && !out[1].title &&
        strstr(out[0].url, "mime=video") && strstr(out[1].url, "mime=audio")) {
        out[0].audio_url = out[1].url;
        out[1].url = NULL;
        source_free(&out[1]);
        n = 1;
    }
    return n;
}

/* ---- HLS playlists saved from the web ---------------------------------------- */

/* a name inside URI="..." on a tag line: where it starts, its length */
static const char *uri_attr(const char *a, const char *b, size_t *len)
{
    const char *u = a;
    while ((u = memchr(u, 'U', (size_t)(b - u))) && u + 5 <= b) {
        if (!memcmp(u, "URI=\"", 5) && (u == a || u[-1] == ',' || u[-1] == ':')) {
            const char *q = memchr(u + 5, '"', (size_t)(b - u - 5));
            if (!q)
                return NULL;
            *len = (size_t)(q - u - 5);
            return u + 5;
        }
        u++;
    }
    return NULL;
}

int sources_hls_relative(const char *text)
{
    const char *p = text, *end = text + strlen(text);
    int hls = 0, rel = 0;
    while (p < end) {
        const char *e = memchr(p, '\n', (size_t)(end - p)), *a = p, *b;
        size_t ulen;
        const char *u;
        if (!e)
            e = end;
        b = e;
        p = e < end ? e + 1 : end;
        while (a < b && (*a == ' ' || *a == '\t'))
            a++;
        while (b > a && (b[-1] == '\r' || b[-1] == ' ' || b[-1] == '\t'))
            b--;
        if (a == b)
            continue;
        if (*a == '#') {
            if (b - a > 7 && !memcmp(a, "#EXT-X-", 7)) {
                hls = 1;
                if ((u = uri_attr(a, b, &ulen)) && ulen && !is_address(u, ulen))
                    rel = 1;
            }
            continue;
        }
        if (!is_address(a, (size_t)(b - a)))
            rel = 1;
    }
    return hls && rel;
}

/* rel resolved against base, appended to out */
static int join(char **out, size_t *n, size_t *cap, const char *base, const char *rel, size_t rlen)
{
    size_t keep, need;
    const char *host = strstr(base, "://"), *path;
    if (is_address(rel, rlen))
        keep = 0;
    else if (rlen >= 2 && rel[0] == '/' && rel[1] == '/')
        keep = (size_t)(host - base) + 1;                 /* "https:" */
    else if (rlen && rel[0] == '/') {
        path = strchr(host + 3, '/');
        keep = path ? (size_t)(path - base) : strlen(base);
    } else {
        size_t q = strcspn(base, "?#");
        keep = q;
        while (keep > (size_t)(host - base) + 3 && base[keep - 1] != '/')
            keep--;
        if (keep <= (size_t)(host - base) + 3) {          /* "https://host" with no path */
            keep = q;
            need = *n + keep + 1 + rlen + 1;
            if (need > *cap) {
                char *t = realloc(*out, *cap = need * 2);
                if (!t) return -1;
                *out = t;
            }
            memcpy(*out + *n, base, keep); *n += keep;
            (*out)[(*n)++] = '/';
            memcpy(*out + *n, rel, rlen); *n += rlen;
            return 0;
        }
    }
    need = *n + keep + rlen + 1;
    if (need > *cap) {
        char *t = realloc(*out, *cap = need * 2);
        if (!t) return -1;
        *out = t;
    }
    memcpy(*out + *n, base, keep); *n += keep;
    memcpy(*out + *n, rel, rlen); *n += rlen;
    return 0;
}

static int put(char **out, size_t *n, size_t *cap, const char *s, size_t len)
{
    if (*n + len + 1 > *cap) {
        char *t = realloc(*out, *cap = (*n + len + 1) * 2);
        if (!t) return -1;
        *out = t;
    }
    memcpy(*out + *n, s, len);
    *n += len;
    return 0;
}

char *sources_hls_rebase(const char *text, const char *base)
{
    const char *p = text, *end = text + strlen(text);
    char *out = NULL;
    size_t n = 0, cap = 0;
    if (!is_address(base, strlen(base)))
        return NULL;
    while (p < end) {
        const char *e = memchr(p, '\n', (size_t)(end - p)), *a = p, *b, *line = p;
        int r = 0;
        if (!e)
            e = end;
        b = e;
        p = e < end ? e + 1 : end;
        while (a < b && (*a == ' ' || *a == '\t'))
            a++;
        while (b > a && (b[-1] == '\r' || b[-1] == ' ' || b[-1] == '\t'))
            b--;
        if (a < b && *a == '#') {
            size_t ulen;
            const char *u = uri_attr(a, b, &ulen);
            if (u && ulen && !is_address(u, ulen))
                r = put(&out, &n, &cap, line, (size_t)(u - line)) || join(&out, &n, &cap, base, u, ulen) ||
                    put(&out, &n, &cap, u + ulen, (size_t)(b - u - ulen));
            else
                r = put(&out, &n, &cap, line, (size_t)(b - line));
        } else if (a < b) {
            r = join(&out, &n, &cap, base, a, (size_t)(b - a));
        }
        if (r || put(&out, &n, &cap, "\n", 1)) {
            free(out);
            return NULL;
        }
    }
    if (put(&out, &n, &cap, "", 0)) {
        free(out);
        return NULL;
    }
    out[n] = 0;
    return out;
}

int sources_from_file(const char *path, source_t *out, int max)
{
    FILE *f = fopen(path, "rb");
    char head[4096];
    size_t got, len;
    char *text;
    int n, hls;

    if (!f)
        return -1;
    got = fread(head, 1, sizeof(head), f);
    if (memchr(head, 0, got) || got == 0) {          /* not text: a video (or empty) */
        fclose(f);
        return 0;
    }
    for (size_t i = 0; i < got; i++) {               /* text has hardly any control codes */
        unsigned char c = (unsigned char)head[i];
        if (c < 32 && c != '\n' && c != '\r' && c != '\t') {
            fclose(f);
            return 0;
        }
    }
    fseek(f, 0, SEEK_END);
    {
        long end = ftell(f);                        /* (-1 past 2GB without large files) */
        len = end < 0 || (unsigned long)end > TEXT_MAX ? TEXT_MAX : (size_t)end;
    }
    fseek(f, 0, SEEK_SET);
    if (!(text = malloc(len + 1))) {
        fclose(f);
        return -1;
    }
    len = fread(text, 1, len, f);
    text[len] = 0;
    fclose(f);
    n = sources_parse(text, len, out, max, &hls);
    free(text);
    return n == 0 && !hls ? -2 : n;                  /* (0: an HLS playlist, played itself) */
}
