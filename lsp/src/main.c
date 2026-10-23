/*
 * hpcs-lsp - a small Language Server for CSV/TSV files.
 *
 * In-tab features:
 *   - diagnostics for ragged rows and unterminated quoted fields
 *   - hover: cell address, column, value, type and a row summary table
 *   - code action / command: open the current file in the HPCSV spreadsheet
 *
 * No dependencies. Talks JSON-RPC over stdio with Content-Length framing.
 */
#define _CRT_SECURE_NO_WARNINGS
#include "hpcs.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define UNICODE
#  define _UNICODE
#  include <windows.h>
#  include <fcntl.h>
#  include <io.h>
#endif

/* --------------------------------------------------------------- buffers */

typedef struct Buf {
    char *p;
    size_t len, cap;
} Buf;

static void bneed(Buf *b, size_t add) {
    if (b->len + add + 1 <= b->cap) return;
    size_t cap = b->cap ? b->cap : 512;
    while (cap < b->len + add + 1) cap *= 2;
    char *p = (char *)realloc(b->p, cap);
    if (!p) return;
    b->p = p;
    b->cap = cap;
}

static void badd(Buf *b, const char *s, size_t n) {
    bneed(b, n);
    if (!b->p) return;
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

static void bputs(Buf *b, const char *s) { badd(b, s, strlen(s)); }

static void bprintf(Buf *b, const char *fmt, ...) {
    char tmp[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof(tmp)) { badd(b, tmp, (size_t)n); return; }
    char *big = (char *)malloc((size_t)n + 1);
    if (!big) return;
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    badd(b, big, (size_t)n);
    free(big);
}

static void bfree(Buf *b) {
    free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
}

static void bjson(Buf *b, const char *s, size_t n) {
    bneed(b, n * 6 + 2);
    badd(b, "\"", 1);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"': badd(b, "\\\"", 2); break;
        case '\\': badd(b, "\\\\", 2); break;
        case '\n': badd(b, "\\n", 2); break;
        case '\r': badd(b, "\\r", 2); break;
        case '\t': badd(b, "\\t", 2); break;
        case '\b': badd(b, "\\b", 2); break;
        case '\f': badd(b, "\\f", 2); break;
        default:
            if (c < 0x20) bprintf(b, "\\u%04x", c);
            else badd(b, (const char *)&s[i], 1);
        }
    }
    badd(b, "\"", 1);
}

/* ------------------------------------------------------------ mini JSON */

static const char *skip_ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

static const char *skip_string(const char *p) {
    if (*p != '"') return p;
    p++;
    while (*p) {
        if (*p == '\\') { if (p[1]) p += 2; else p++; continue; }
        if (*p == '"') return p + 1;
        p++;
    }
    return p;
}

static const char *skip_value(const char *p) {
    p = skip_ws(p);
    if (*p == '"') return skip_string(p);
    if (*p == '{' || *p == '[') {
        char open = *p;
        char close = (open == '{') ? '}' : ']';
        int depth = 0;
        while (*p) {
            if (*p == '"') { p = skip_string(p); continue; }
            if (*p == open) depth++;
            else if (*p == close) { depth--; if (depth == 0) return p + 1; }
            p++;
        }
        return p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']' && !isspace((unsigned char)*p)) p++;
    return p;
}

/* Returns the value start for `key` inside the object at p. */
static const char *find_key(const char *p, const char *key, const char **out_end) {
    p = skip_ws(p);
    if (*p != '{') return NULL;
    p = skip_ws(p + 1);
    size_t klen = strlen(key);
    while (*p && *p != '}') {
        if (*p != '"') { p = skip_value(p); break; }
        const char *ks = p + 1;
        const char *ke = skip_string(p) - 1;
        p = skip_ws(skip_string(p));
        if (*p != ':') return NULL;
        p = skip_ws(p + 1);
        const char *ve = skip_value(p);
        if ((size_t)(ke - ks) == klen && memcmp(ks, key, klen) == 0) {
            if (out_end) *out_end = ve;
            return p;
        }
        p = skip_ws(ve);
        if (*p == ',') p = skip_ws(p + 1);
    }
    return NULL;
}

static int hex4(const char *p) {
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return -1;
    }
    return v;
}

static void put_utf8(Buf *b, unsigned cp) {
    char out[4];
    int n;
    if (cp < 0x80) { out[0] = (char)cp; n = 1; }
    else if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    badd(b, out, (size_t)n);
}

static char *json_dup(const char *p, size_t *out_len) {
    p = skip_ws(p);
    if (*p != '"') return NULL;
    Buf b = {0};
    p++;
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            char c = *p;
            switch (c) {
            case 'n': badd(&b, "\n", 1); p++; break;
            case 'r': badd(&b, "\r", 1); p++; break;
            case 't': badd(&b, "\t", 1); p++; break;
            case 'b': badd(&b, "\b", 1); p++; break;
            case 'f': badd(&b, "\f", 1); p++; break;
            case '/': badd(&b, "/", 1); p++; break;
            case '"': badd(&b, "\"", 1); p++; break;
            case '\\': badd(&b, "\\", 1); p++; break;
            case 'u': {
                int cp = hex4(p + 1);
                if (cp < 0) { p++; break; }
                p += 5;
                if (cp >= 0xD800 && cp <= 0xDBFF && p[0] == '\\' && p[1] == 'u') {
                    int lo = hex4(p + 2);
                    if (lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        p += 6;
                    }
                }
                put_utf8(&b, (unsigned)cp);
                break;
            }
            default: if (c) { badd(&b, &c, 1); p++; } else p++;
            }
        } else {
            badd(&b, p, 1);
            p++;
        }
    }
    char nul = 0;
    badd(&b, &nul, 1);
    if (out_len) *out_len = b.len ? b.len - 1 : 0;
    return b.p;
}

static int json_int(const char *p, int *out) {
    p = skip_ws(p);
    char *end = NULL;
    long v = strtol(p, &end, 10);
    if (end == p) return 0;
    *out = (int)v;
    return 1;
}

/* --------------------------------------------------------------- output */

static void send_message(const char *json, size_t len) {
#ifdef _WIN32
    fprintf(stdout, "Content-Length: %zu\r\n\r\n", len);
#else
    fprintf(stdout, "Content-Length: %zu\r\n\r\n", len);
#endif
    fwrite(json, 1, len, stdout);
    fflush(stdout);
}

static void respond_raw(const char *id, size_t idlen, const char *result) {
    Buf b = {0};
    bputs(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
    badd(&b, id, idlen);
    bputs(&b, ",\"result\":");
    bputs(&b, result);
    bputs(&b, "}");
    send_message(b.p, b.len);
    bfree(&b);
}

static void respond_json(const char *id, size_t idlen, const Buf *result) {
    respond_raw(id, idlen, result->p ? result->p : "null");
}

static void notify(const char *method, const Buf *params) {
    Buf b = {0};
    bputs(&b, "{\"jsonrpc\":\"2.0\",\"method\":\"");
    bputs(&b, method);
    bputs(&b, "\",\"params\":");
    bputs(&b, params->p ? params->p : "{}");
    bputs(&b, "}");
    send_message(b.p, b.len);
    bfree(&b);
}

/* ------------------------------------------------------------- document */

#define HPCS_MAX_DOCS 32

typedef struct DocEntry {
    char *uri;
    HpDoc doc;
    int used;
} DocEntry;

static DocEntry g_docs[HPCS_MAX_DOCS];

static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static DocEntry *doc_find(const char *uri) {
    for (int i = 0; i < HPCS_MAX_DOCS; i++)
        if (g_docs[i].used && g_docs[i].uri && strcmp(g_docs[i].uri, uri) == 0)
            return &g_docs[i];
    return NULL;
}

static DocEntry *doc_slot(const char *uri) {
    DocEntry *e = doc_find(uri);
    if (e) return e;
    for (int i = 0; i < HPCS_MAX_DOCS; i++) {
        if (!g_docs[i].used) { e = &g_docs[i]; break; }
    }
    if (!e) e = &g_docs[0];
    if (e->used) {
        hpcs_free(&e->doc);
        free(e->uri);
    }
    memset(e, 0, sizeof(*e));
    hpcs_init(&e->doc);
    e->used = 1;
    e->uri = xstrdup(uri);
    return e;
}

static void doc_clear(DocEntry *e) {
    if (!e) return;
    if (e->used) hpcs_free(&e->doc);
    free(e->uri);
    memset(e, 0, sizeof(*e));
    hpcs_init(&e->doc);
}

static DocEntry *doc_set(const char *uri, const char *text, size_t len) {
    DocEntry *e = doc_slot(uri);
    hpcs_free(&e->doc);
    hpcs_init(&e->doc);
    char *copy = (char *)malloc(len + 1);
    if (!copy) return NULL;
    memcpy(copy, text, len);
    copy[len] = '\0';
    if (hpcs_parse(&e->doc, copy, len) != HPCS_OK) {
        hpcs_free(&e->doc);
        hpcs_init(&e->doc);
        return NULL;
    }
    return e;
}

static int utf8_len_at(const char *s, size_t n, size_t i) {
    unsigned char c = (unsigned char)s[i];
    int len = 1;
    if ((c & 0xE0) == 0xC0) len = 2;
    else if ((c & 0xF0) == 0xE0) len = 3;
    else if ((c & 0xF8) == 0xF0) len = 4;
    if (i + (size_t)len > n) len = 1;
    return len;
}

static int utf16_units(const char *s, size_t n, size_t i) {
    unsigned char c = (unsigned char)s[i];
    return (c >= 0xF0) ? 2 : 1;
}

static size_t offset_from_position(const char *s, size_t n, int line, int character) {
    size_t off = 0;
    int l = 0;
    while (l < line && off < n) {
        if (s[off] == '\n') l++;
        off++;
    }
    int u = 0;
    while (off < n && u < character && s[off] != '\n') {
        int units = utf16_units(s, n, off);
        if (u + units > character) break;
        u += units;
        off += (size_t)utf8_len_at(s, n, off);
    }
    return off;
}

static void position_from_offset(const char *s, size_t n, size_t off, int *line, int *character) {
    if (off > n) off = n;
    int l = 0;
    size_t ls = 0;
    for (size_t i = 0; i < off; i++) {
        if (s[i] == '\n') { l++; ls = i + 1; }
    }
    int u = 0;
    for (size_t i = ls; i < off;) {
        u += utf16_units(s, n, i);
        i += (size_t)utf8_len_at(s, n, i);
    }
    *line = l;
    *character = u;
}

static void cell_span(const HpDoc *d, uint32_t row, uint32_t col, size_t *start, size_t *end) {
    const HpCell *c = hpcs_cell_at(d, row, col);
    if (!c) { *start = *end = 0; return; }
    size_t s = c->quoted ? (size_t)c->off - 1 : (size_t)c->off;
    *start = s;
    *end = s + c->raw_len;
}

/* ---------------------------------------------------------- diagnostics */

static int utf16_cols(const char *s, size_t n, size_t a, size_t b) {
    int u = 0;
    for (size_t i = a; i < b;) {
        u += utf16_units(s, n, i);
        i += (size_t)utf8_len_at(s, n, i);
    }
    return u;
}

static void add_diag_at(Buf *b, int *count, int line, int endch, const char *message) {
    if (*count >= 200) return;
    if (*count) bputs(b, ",");
    bputs(b, "{\"range\":{\"start\":{\"line\":");
    bprintf(b, "%d", line);
    bputs(b, ",\"character\":0},\"end\":{\"line\":");
    bprintf(b, "%d", line);
    bputs(b, ",\"character\":");
    bprintf(b, "%d", endch);
    bputs(b, "}},\"severity\":2,\"source\":\"hpcs\",\"message\":");
    bjson(b, message, strlen(message));
    bputs(b, "}");
    (*count)++;
}

static void publish_diagnostics(DocEntry *e) {
    if (!e || !e->used || !e->uri) return;
    Buf params = {0};
    bputs(&params, "{\"uri\":");
    bjson(&params, e->uri, strlen(e->uri));
    bputs(&params, ",\"diagnostics\":[");
    int count = 0;
    {
        const char *s = e->doc.buf;
        size_t n = e->doc.len;
        if (e->doc.first_unterminated != UINT32_MAX) {
            int line = 0, ch = 0;
            position_from_offset(s, n, e->doc.first_unterminated, &line, &ch);
            size_t ls = e->doc.first_unterminated;
            while (ls > 0 && s[ls - 1] != '\n') ls--;
            size_t le = e->doc.first_unterminated;
            while (le < n && s[le] != '\n') le++;
            char msg[128];
            snprintf(msg, sizeof(msg), "Unterminated quoted field (%u in file)",
                     e->doc.unterminated);
            add_diag_at(&params, &count, line, utf16_cols(s, n, ls, le), msg);
        }
        if (e->doc.n_rows > 1) {
            uint32_t ref = e->doc.rows[0].count;
            size_t scan = 0;
            int line = 0;
            for (uint32_t r = 1; r < e->doc.n_rows && count < 200; r++) {
                uint32_t have = e->doc.rows[r].count;
                size_t st = 0, en = 0;
                cell_span(&e->doc, r, 0, &st, &en);
                while (scan < st && scan < n) {
                    if (s[scan] == '\n') line++;
                    scan++;
                }
                if (have != ref) {
                    size_t ls = st;
                    while (ls > 0 && s[ls - 1] != '\n') ls--;
                    size_t le = ls;
                    while (le < n && s[le] != '\n') le++;
                    char msg[160];
                    snprintf(msg, sizeof(msg), "Row %u has %u fields, header has %u",
                             r + 1, have, ref);
                    add_diag_at(&params, &count, line, utf16_cols(s, n, ls, le), msg);
                }
            }
        }
    }
    bputs(&params, "]}");
    notify("textDocument/publishDiagnostics", &params);
    bfree(&params);
}

static void clear_diagnostics(const char *uri) {
    if (!uri) return;
    Buf params = {0};
    bputs(&params, "{\"uri\":");
    bjson(&params, uri, strlen(uri));
    bputs(&params, ",\"diagnostics\":[]}");
    notify("textDocument/publishDiagnostics", &params);
    bfree(&params);
}

/* --------------------------------------------------------------- hover */

static void md_cell(Buf *b, const char *s, size_t n) {
    if (n > 80) n = 80;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '|') bputs(b, "\\|");
        else if (c == '\n' || c == '\r' || c == '\t') bputs(b, " ");
        else badd(b, &c, 1);
    }
    if (n == 80) bputs(b, "\u2026");
}

static void handle_hover(const char *id, size_t idlen, const char *params, DocEntry *e) {
    if (!e || !e->used || e->doc.n_rows == 0) { respond_raw(id, idlen, "null"); return; }
    const char *pos = find_key(params, "position", NULL);
    if (!pos) { respond_raw(id, idlen, "null"); return; }
    const char *lv = find_key(pos, "line", NULL);
    const char *cv = find_key(pos, "character", NULL);
    int line = 0, ch = 0;
    if (!lv || !cv || !json_int(lv, &line) || !json_int(cv, &ch)) {
        respond_raw(id, idlen, "null");
        return;
    }
    size_t off = offset_from_position(e->doc.buf, e->doc.len, line, ch);

    int found_row = -1, found_col = -1;
    for (uint32_t r = 0; r < e->doc.n_rows && found_row < 0; r++) {
        for (uint32_t c = 0; c < e->doc.rows[r].count; c++) {
            size_t st = 0, en = 0;
            cell_span(&e->doc, r, c, &st, &en);
            if (off >= st && off <= en) { found_row = (int)r; found_col = (int)c; break; }
            if (off < st) break;
        }
    }
    if (found_row < 0) { respond_raw(id, idlen, "null"); return; }
    if (found_col >= (int)e->doc.rows[found_row].count) found_col = 0;

    char letter[8];
    hpcs_column_letter((uint32_t)found_col, letter, sizeof(letter));

    size_t vlen = 0;
    const char *val = hpcs_cell_text(&e->doc, (uint32_t)found_row, (uint32_t)found_col, &vlen);
    int numeric = hpcs_cell_is_numeric(val, vlen);

    Buf md = {0};
    bputs(&md, "**HPCSV** \u00b7 row ");
    bprintf(&md, "%d", found_row + 1);
    bputs(&md, ", column ");
    bputs(&md, letter);
    if (e->doc.n_rows > 0 && (uint32_t)found_col < e->doc.rows[0].count) {
        size_t hn = 0;
        const char *h = hpcs_cell_text(&e->doc, 0, (uint32_t)found_col, &hn);
        if (hn) {
            bputs(&md, " \u2014 ");
            md_cell(&md, h, hn);
        }
    }
    bputs(&md, "\n\nType: ");
    bputs(&md, vlen == 0 ? "empty" : (numeric ? "number" : "text"));
    bputs(&md, " \u00b7 length ");
    bprintf(&md, "%zu", vlen);
    bputs(&md, "\n\n");

    /* row summary table */
    bputs(&md, "| Column | Value |\n|---|---|\n");
    uint32_t cols = e->doc.rows[found_row].count;
    uint32_t maxc = cols < 12 ? cols : 12;
    for (uint32_t c = 0; c < maxc; c++) {
        bputs(&md, "| ");
        if (e->doc.n_rows > 0 && c < e->doc.rows[0].count) {
            size_t hn = 0;
            const char *h = hpcs_cell_text(&e->doc, 0, c, &hn);
            if (hn) md_cell(&md, h, hn);
            else { char l[8]; hpcs_column_letter(c, l, sizeof(l)); bputs(&md, l); }
        } else {
            char l[8];
            hpcs_column_letter(c, l, sizeof(l));
            bputs(&md, l);
        }
        bputs(&md, " | ");
        size_t cn = 0;
        const char *ct = hpcs_cell_text(&e->doc, (uint32_t)found_row, c, &cn);
        md_cell(&md, ct, cn);
        bputs(&md, " |\n");
    }
    if (cols > maxc) {
        bputs(&md, "\n_");
        bprintf(&md, "%u", cols - maxc);
        bputs(&md, " more columns_");
    }

    size_t st = 0, en = 0;
    cell_span(&e->doc, (uint32_t)found_row, (uint32_t)found_col, &st, &en);
    int sl = 0, sc = 0, el = 0, ec = 0;
    position_from_offset(e->doc.buf, e->doc.len, st, &sl, &sc);
    position_from_offset(e->doc.buf, e->doc.len, en, &el, &ec);

    Buf res = {0};
    bputs(&res, "{\"contents\":{\"kind\":\"markdown\",\"value\":");
    bjson(&res, md.p ? md.p : "", md.len);
    bputs(&res, "},\"range\":{\"start\":{\"line\":");
    bprintf(&res, "%d", sl);
    bputs(&res, ",\"character\":");
    bprintf(&res, "%d", sc);
    bputs(&res, "},\"end\":{\"line\":");
    bprintf(&res, "%d", el);
    bputs(&res, ",\"character\":");
    bprintf(&res, "%d", ec);
    bputs(&res, "}}}");
    respond_json(id, idlen, &res);
    bfree(&res);
    bfree(&md);
}

/* -------------------------------------------------------------- actions */

static char *uri_to_path(const char *uri) {
    const char *p = uri;
    if (strncmp(p, "file://", 7) == 0) p += 7;
    Buf b = {0};
    if (*p == '/') {
        /* /C:/dir/file -> C:/dir/file */
        if (isalpha((unsigned char)p[1]) && p[2] == ':') p++;
    }
    while (*p) {
        if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char h[3] = { p[1], p[2], 0 };
            char c = (char)strtol(h, NULL, 16);
            badd(&b, &c, 1);
            p += 3;
        } else {
            char c = (*p == '/') ? '\\' : *p;
            badd(&b, &c, 1);
            p++;
        }
    }
    char nul = 0;
    badd(&b, &nul, 1);
    return b.p;
}

static int launch_app(const char *uri) {
#ifdef _WIN32
    char *path = uri_to_path(uri);
    if (!path) return 0;
    int wn = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (wn <= 0) { free(path); return 0; }
    wchar_t *wpath = (wchar_t *)malloc((size_t)wn * sizeof(wchar_t));
    if (!wpath) { free(path); return 0; }
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, wn);

    wchar_t exe[MAX_PATH * 2];
    exe[0] = 0;
    wchar_t envbuf[32];
    DWORD el = GetEnvironmentVariableW(L"HPCSV_BIN", envbuf, 32);
    if (el > 0 && el < 32) wcscpy(exe, envbuf);
    if (!exe[0]) {
        wchar_t self[MAX_PATH * 2];
        DWORD n = GetModuleFileNameW(NULL, self, MAX_PATH * 2);
        if (n > 0) {
            wchar_t *slash = wcsrchr(self, L'\\');
            if (slash) {
                *slash = 0;
                swprintf(exe, MAX_PATH * 2, L"%ls\\HPCSV.exe", self);
                if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) exe[0] = 0;
            }
        }
    }
    if (!exe[0]) {
        wchar_t found[MAX_PATH * 2];
        if (SearchPathW(NULL, L"HPCSV.exe", NULL, MAX_PATH * 2, found, NULL))
            wcscpy(exe, found);
    }
    if (!exe[0]) {
        free(wpath);
        free(path);
        return 0;
    }

    wchar_t cmd[4096];
    swprintf(cmd, 4096, L"\"%ls\" \"%ls\"", exe, wpath);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    BOOL ok = CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    if (ok) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    free(wpath);
    free(path);
    return ok ? 1 : 0;
#else
    (void)uri;
    return 0;
#endif
}

static void handle_code_action(const char *id, size_t idlen, const char *params) {
    const char *td = find_key(params, "textDocument", NULL);
    const char *uri = td ? find_key(td, "uri", NULL) : NULL;
    if (!uri) { respond_raw(id, idlen, "[]"); return; }
    Buf r = {0};
    bputs(&r, "[{\"title\":\"HPCSV: open in spreadsheet\",\"kind\":\"source\",\"isPreferred\":true,"
              "\"command\":{\"title\":\"Open in HPCSV\",\"command\":\"hpcs.open\",\"arguments\":[");
    badd(&r, uri, (size_t)(skip_value(uri) - uri));
    bputs(&r, "]}}]");
    respond_json(id, idlen, &r);
    bfree(&r);
}

static void handle_execute(const char *id, size_t idlen, const char *params) {
    const char *cmd = find_key(params, "command", NULL);
    char *name = cmd ? json_dup(cmd, NULL) : NULL;
    int is_open = name && strcmp(name, "hpcs.open") == 0;
    int launched = 0;
    if (is_open) {
        const char *args = find_key(params, "arguments", NULL);
        if (args) {
            const char *p = skip_ws(args);
            if (*p == '[') {
                p = skip_ws(p + 1);
                char *uri = json_dup(p, NULL);
                if (uri) { launched = launch_app(uri); free(uri); }
            }
        }
    }
    free(name);
    respond_raw(id, idlen, "null");
    if (is_open && !launched) {
        Buf params2 = {0};
        bputs(&params2, "{\"type\":1,\"message\":\"HPCSV executable not found. Build it with `zig build` and add zig-out/bin to PATH (or set HPCSV_BIN).\"}");
        notify("window/showMessage", &params2);
        bfree(&params2);
    }
}

/* ---------------------------------------------------------------- main */

static char *read_message(size_t *out_len) {
    char line[512];
    size_t content_length = 0;
    int any = 0;
    while (fgets(line, sizeof(line), stdin)) {
        any = 1;
        if (line[0] == '\r' || line[0] == '\n') break;
        if (_strnicmp(line, "Content-Length:", 15) == 0)
            content_length = (size_t)strtoul(line + 15, NULL, 10);
    }
    if (!any || content_length == 0) return NULL;
    char *buf = (char *)malloc(content_length + 1);
    if (!buf) return NULL;
    size_t got = 0;
    while (got < content_length) {
        size_t n = fread(buf + got, 1, content_length - got, stdin);
        if (n == 0) break;
        got += n;
    }
    if (got < content_length) { free(buf); return NULL; }
    buf[got] = '\0';
    *out_len = got;
    return buf;
}

static char *params_uri(const char *params) {
    if (!params) return NULL;
    const char *td = find_key(params, "textDocument", NULL);
    const char *uri = td ? find_key(td, "uri", NULL) : NULL;
    return uri ? json_dup(uri, NULL) : NULL;
}

int main(void) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    for (int i = 0; i < HPCS_MAX_DOCS; i++) hpcs_init(&g_docs[i].doc);

    for (;;) {
        size_t len = 0;
        char *msg = read_message(&len);
        if (!msg) break;

        const char *id = find_key(msg, "id", NULL);
        size_t idlen = id ? (size_t)(skip_value(id) - id) : 0;
        const char *method = find_key(msg, "method", NULL);
        char *meth = method ? json_dup(method, NULL) : NULL;
        const char *params = find_key(msg, "params", NULL);

        if (!meth) { free(msg); continue; }

        if (strcmp(meth, "initialize") == 0) {
            const char *caps =
                "{\"capabilities\":{\"textDocumentSync\":1,\"hoverProvider\":true,"
                "\"codeActionProvider\":true,"
                "\"executeCommandProvider\":{\"commands\":[\"hpcs.open\"]}},"
                "\"serverInfo\":{\"name\":\"hpcs\",\"version\":\"0.1.0\"}}";
            if (id) respond_raw(id, idlen, caps);
        } else if (strcmp(meth, "initialized") == 0) {
            /* no-op */
        } else if (strcmp(meth, "shutdown") == 0) {
            if (id) respond_raw(id, idlen, "null");
        } else if (strcmp(meth, "exit") == 0) {
            free(meth);
            free(msg);
            break;
        } else if (strcmp(meth, "textDocument/didOpen") == 0) {
            char *u = params_uri(params);
            if (u) {
                const char *td = find_key(params, "textDocument", NULL);
                const char *text = td ? find_key(td, "text", NULL) : NULL;
                if (text) {
                    size_t tl = 0;
                    char *t = json_dup(text, &tl);
                    if (t) {
                        DocEntry *e = doc_set(u, t, tl);
                        publish_diagnostics(e);
                        free(t);
                    }
                }
            }
            free(u);
        } else if (strcmp(meth, "textDocument/didChange") == 0) {
            char *u = params_uri(params);
            if (u) {
                const char *changes = find_key(params, "contentChanges", NULL);
                if (changes) {
                    const char *p = skip_ws(changes);
                    if (*p == '[') {
                        p = skip_ws(p + 1);
                        const char *text = find_key(p, "text", NULL);
                        if (text) {
                            size_t tl = 0;
                            char *t = json_dup(text, &tl);
                            if (t) {
                                DocEntry *e = doc_set(u, t, tl);
                                publish_diagnostics(e);
                                free(t);
                            }
                        }
                    }
                }
            }
            free(u);
        } else if (strcmp(meth, "textDocument/didClose") == 0) {
            char *u = params_uri(params);
            if (u) {
                clear_diagnostics(u);
                DocEntry *e = doc_find(u);
                if (e) doc_clear(e);
            }
            free(u);
        } else if (strcmp(meth, "textDocument/hover") == 0) {
            char *u = params_uri(params);
            DocEntry *e = u ? doc_find(u) : NULL;
            if (id && params) handle_hover(id, idlen, params, e);
            else if (id) respond_raw(id, idlen, "null");
            free(u);
        } else if (strcmp(meth, "textDocument/codeAction") == 0) {
            if (id && params) handle_code_action(id, idlen, params);
            else if (id) respond_raw(id, idlen, "[]");
        } else if (strcmp(meth, "workspace/executeCommand") == 0) {
            if (id && params) handle_execute(id, idlen, params);
            else if (id) respond_raw(id, idlen, "null");
        } else {
            if (id) respond_raw(id, idlen, "null");
        }

        free(meth);
        free(msg);
    }

    for (int i = 0; i < HPCS_MAX_DOCS; i++) doc_clear(&g_docs[i]);
    return 0;
}


