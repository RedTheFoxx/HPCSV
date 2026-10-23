#define _CRT_SECURE_NO_WARNINGS
#include "hpcs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <unistd.h>
#endif

#define HPCS_MIN(a, b) ((a) < (b) ? (a) : (b))

const char *hpcs_version(void) { return "0.1.0"; }

void hpcs_init(HpDoc *doc) { memset(doc, 0, sizeof(*doc)); }

static void free_doc_arrays(HpDoc *doc) {
    free(doc->buf);
    free(doc->cells);
    free(doc->rows);
    doc->buf = NULL;
    doc->cells = NULL;
    doc->rows = NULL;
    doc->len = doc->cap = 0;
    doc->n_cells = doc->cap_cells = 0;
    doc->n_rows = doc->cap_rows = 0;
}

void hpcs_free(HpDoc *doc) {
    free_doc_arrays(doc);
    free(doc->scratch);
    doc->scratch = NULL;
    doc->scratch_cap = 0;
}

static int ensure_scratch(HpDoc *doc, size_t need) {
    if (doc->scratch_cap >= need && doc->scratch) return 1;
    size_t cap = doc->scratch_cap ? doc->scratch_cap : 256;
    while (cap < need) cap *= 2;
    char *p = (char *)realloc(doc->scratch, cap);
    if (!p) return 0;
    doc->scratch = p;
    doc->scratch_cap = cap;
    return 1;
}

static int reserve_cells(HpDoc *doc, uint32_t need) {
    if (need <= doc->cap_cells) return 1;
    uint32_t cap = doc->cap_cells ? doc->cap_cells : 1024;
    while (cap < need) {
        if (cap > UINT32_MAX / 2) { cap = need; break; }
        cap *= 2;
    }
    HpCell *p = (HpCell *)realloc(doc->cells, (size_t)cap * sizeof(HpCell));
    if (!p) return 0;
    doc->cells = p;
    doc->cap_cells = cap;
    return 1;
}

static int reserve_rows(HpDoc *doc, uint32_t need) {
    if (need <= doc->cap_rows) return 1;
    uint32_t cap = doc->cap_rows ? doc->cap_rows : 256;
    while (cap < need) {
        if (cap > UINT32_MAX / 2) { cap = need; break; }
        cap *= 2;
    }
    HpRow *p = (HpRow *)realloc(doc->rows, (size_t)cap * sizeof(HpRow));
    if (!p) return 0;
    doc->rows = p;
    doc->cap_rows = cap;
    return 1;
}

static int push_row(HpDoc *doc, uint32_t first, uint32_t count) {
    if (!reserve_rows(doc, doc->n_rows + 1)) return 0;
    doc->rows[doc->n_rows].first = first;
    doc->rows[doc->n_rows].count = count;
    doc->n_rows++;
    if (count > doc->max_cols) doc->max_cols = count;
    return 1;
}

static int push_cell(HpDoc *doc, uint32_t off, uint32_t len, uint32_t raw_len, int quoted) {
    if (!reserve_cells(doc, doc->n_cells + 1)) return 0;
    HpCell *c = &doc->cells[doc->n_cells++];
    c->off = off;
    c->len = len;
    c->raw_len = raw_len;
    c->quoted = (uint8_t)(quoted ? 1 : 0);
    c->pad[0] = c->pad[1] = c->pad[2] = 0;
    return 1;
}

static int sniff_score(const char *b, size_t n, size_t start, char cand) {
    int counts[64];
    int nrec = 0;
    size_t i = start;
    size_t lim = n < 262144 ? n : 262144;
    while (i < lim && nrec < 64) {
        int cnt = 0;
        int quoted = 0;
        while (i < lim && b[i] != '\n') {
            unsigned char ch = (unsigned char)b[i];
            if (quoted) {
                if (ch == '"') {
                    if (i + 1 < lim && b[i + 1] == '"') i++;
                    else quoted = 0;
                }
            } else if (ch == '"') {
                quoted = 1;
            } else if (ch == (unsigned char)cand) {
                cnt++;
            }
            i++;
        }
        if (i < lim && b[i] == '\n') i++;
        counts[nrec++] = cnt;
    }
    if (nrec == 0) return 0;
    /* Most frequent field count; consistency wins, then width. */
    int best_count = 0, best_freq = 0;
    for (int k = 0; k < nrec; k++) {
        int f = 0;
        for (int j = 0; j < nrec; j++)
            if (counts[j] == counts[k]) f++;
        if (f > best_freq || (f == best_freq && counts[k] > best_count)) {
            best_freq = f;
            best_count = counts[k];
        }
    }
    if (best_count == 0) return 0;
    return best_freq * 1000 + best_count;
}

static char sniff_delimiter(const char *b, size_t n, size_t start) {
    static const char cands[] = { ',', ';', '\t', '|' };
    char best = ',';
    int best_score = -1;
    for (size_t k = 0; k < sizeof(cands); k++) {
        int s = sniff_score(b, n, start, cands[k]);
        if (s > best_score) {
            best_score = s;
            best = cands[k];
        }
    }
    return best;
}

HpStatus hpcs_parse(HpDoc *doc, char *buf, size_t len) {
    if (!buf) return HPCS_ERR_ARG;
    if (len > (size_t)UINT32_MAX - 1) {
        free(buf);
        return HPCS_ERR_TOO_LARGE;
    }

    free_doc_arrays(doc);
    doc->buf = buf;
    doc->len = len;
    doc->cap = len + 1;
    buf[len] = '\0';
    doc->n_cells = doc->n_rows = 0;
    doc->max_cols = doc->ragged_rows = doc->unterminated = doc->warnings = 0;
    doc->first_unterminated = UINT32_MAX;
    doc->crlf = 0;
    doc->final_newline = 0;
    doc->dirty = 0;

    size_t start = 0;
    if (len >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB &&
        (unsigned char)buf[2] == 0xBF) {
        doc->bom = 3;
        start = 3;
    } else {
        doc->bom = 0;
    }
    doc->delim = sniff_delimiter(buf, len, start);
    const char delim = doc->delim;

    size_t i = start;
    while (i < len) {
        uint32_t first = doc->n_cells;
        for (;;) {
            size_t cell_start = i;
            size_t content_start = i;
            size_t content_len = 0;
            int quoted = 0;

            if (i < len && buf[i] == '"') {
                quoted = 1;
                content_start = i + 1;
                size_t j = content_start;
                int closed = 0;
                while (j < len) {
                    if (buf[j] == '"') {
                        if (j + 1 < len && buf[j + 1] == '"') { j += 2; continue; }
                        closed = 1;
                        j++;
                        break;
                    }
                    j++;
                }
                content_len = (closed ? j - 1 : len) - content_start;
                if (!closed) {
                    if (doc->unterminated == 0) doc->first_unterminated = (uint32_t)cell_start;
                    doc->unterminated++;
                    doc->warnings++;
                }
                /* Be liberal: accept bytes between the closing quote and the
                 * delimiter (kept in the raw span so edits still round-trip). */
                while (j < len && buf[j] != delim && buf[j] != '\n' && buf[j] != '\r') {
                    j++;
                    doc->warnings++;
                }
                i = j;
            } else {
                while (i < len && buf[i] != delim && buf[i] != '\n' && buf[i] != '\r') i++;
                content_len = i - cell_start;
            }

            if (!push_cell(doc, (uint32_t)content_start, (uint32_t)content_len,
                           (uint32_t)(i - cell_start), quoted))
                return HPCS_ERR_NOMEM;

            if (i < len && buf[i] == delim) { i++; continue; }
            break;
        }

        if (i < len) {
            if (buf[i] == '\r') {
                doc->crlf = 1;
                i++;
                if (i < len && buf[i] == '\n') i++;
            } else if (buf[i] == '\n') {
                i++;
            }
            doc->final_newline = (i >= len);
        }

        if (!push_row(doc, first, doc->n_cells - first)) return HPCS_ERR_NOMEM;
    }

    if (doc->n_rows > 1) {
        uint32_t ref = doc->rows[0].count;
        for (uint32_t r = 1; r < doc->n_rows; r++)
            if (doc->rows[r].count != ref) doc->ragged_rows++;
    }
    return HPCS_OK;
}

static int read_file_utf8(const char *path, char **out, size_t *out_len) {
#ifdef _WIN32
    wchar_t *wpath = NULL;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (wlen <= 0) return 0;
    wpath = (wchar_t *)malloc((size_t)wlen * sizeof(wchar_t));
    if (!wpath) return 0;
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, wlen);

    HANDLE h = CreateFileW(wpath, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(wpath);
    if (h == INVALID_HANDLE_VALUE) return 0;

    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 0 || size.QuadPart > (LONGLONG)UINT32_MAX - 8) {
        CloseHandle(h);
        return 0;
    }
    size_t len = (size_t)size.QuadPart;
    char *buf = (char *)malloc(len + 1);
    if (!buf) { CloseHandle(h); return 0; }
    size_t got = 0;
    while (got < len) {
        DWORD chunk = (DWORD)HPCS_MIN((size_t)0x10000000, len - got);
        DWORD read = 0;
        if (!ReadFile(h, buf + got, chunk, &read, NULL) || read == 0) break;
        got += read;
    }
    CloseHandle(h);
    if (got != len) { free(buf); return 0; }
    buf[len] = '\0';
    *out = buf;
    *out_len = len;
    return 1;
#else
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long sz = ftell(f);
    if (sz < 0 || (unsigned long)sz > UINT32_MAX - 8) { fclose(f); return 0; }
    rewind(f);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return 0; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(buf); return 0; }
    buf[got] = '\0';
    *out = buf;
    *out_len = got;
    return 1;
#endif
}

HpStatus hpcs_load(HpDoc *doc, const char *utf8_path) {
    char *buf = NULL;
    size_t len = 0;
    if (!read_file_utf8(utf8_path, &buf, &len)) return HPCS_ERR_IO;
    return hpcs_parse(doc, buf, len);
}

static int write_file_atomic(const char *path, const char *data, size_t len) {
#ifdef _WIN32
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (wlen <= 0) return 0;
    wchar_t *wpath = (wchar_t *)malloc((size_t)(wlen + 32) * sizeof(wchar_t));
    if (!wpath) return 0;
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, wlen);

    wchar_t tmp[MAX_PATH + 64];
    swprintf(tmp, MAX_PATH + 63, L"%ls.hpcs-%lu.tmp", wpath, (unsigned long)GetCurrentProcessId());
    tmp[MAX_PATH + 63] = L'\0';

    HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { free(wpath); return 0; }
    size_t done = 0;
    int ok = 1;
    while (done < len) {
        DWORD chunk = (DWORD)HPCS_MIN((size_t)0x10000000, len - done);
        DWORD wrote = 0;
        if (!WriteFile(h, data + done, chunk, &wrote, NULL) || wrote == 0) { ok = 0; break; }
        done += wrote;
    }
    if (ok) FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) { DeleteFileW(tmp); free(wpath); return 0; }

    if (!MoveFileExW(tmp, wpath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp);
        free(wpath);
        return 0;
    }
    free(wpath);
    return 1;
#else
    size_t plen = strlen(path);
    char *tmp = (char *)malloc(plen + 32);
    if (!tmp) return 0;
    snprintf(tmp, plen + 32, "%s.hpcs-%lu.tmp", path, (unsigned long)getpid());
    FILE *f = fopen(tmp, "wb");
    if (!f) { free(tmp); return 0; }
    int ok = fwrite(data, 1, len, f) == len;
    if (fclose(f) != 0) ok = 0;
    if (!ok || rename(tmp, path) != 0) { remove(tmp); free(tmp); return 0; }
    free(tmp);
    return 1;
#endif
}

HpStatus hpcs_save(HpDoc *doc, const char *utf8_path) {
    if (!write_file_atomic(utf8_path, doc->buf, doc->len)) return HPCS_ERR_IO;
    doc->dirty = 0;
    return HPCS_OK;
}

uint32_t hpcs_row_count(const HpDoc *doc) { return doc->n_rows; }

uint32_t hpcs_row_cols(const HpDoc *doc, uint32_t row) {
    if (row >= doc->n_rows) return 0;
    return doc->rows[row].count;
}

const HpCell *hpcs_cell_at(const HpDoc *doc, uint32_t row, uint32_t col) {
    if (row >= doc->n_rows) return NULL;
    const HpRow *r = &doc->rows[row];
    if (col >= r->count) return NULL;
    return &doc->cells[r->first + col];
}

const char *hpcs_cell_raw(const HpDoc *doc, uint32_t row, uint32_t col, size_t *len) {
    const HpCell *c = hpcs_cell_at(doc, row, col);
    if (!c) { if (len) *len = 0; return ""; }
    if (len) *len = c->raw_len;
    return doc->buf + (c->quoted ? c->off - 1 : c->off);
}

const char *hpcs_cell_text(HpDoc *doc, uint32_t row, uint32_t col, size_t *len) {
    const HpCell *c = hpcs_cell_at(doc, row, col);
    if (!c) { if (len) *len = 0; return ""; }
    if (!c->quoted) {
        if (len) *len = c->len;
        return doc->buf + c->off;
    }
    if (!ensure_scratch(doc, (size_t)c->len + 1)) {
        if (len) *len = 0;
        return "";
    }
    const char *src = doc->buf + c->off;
    char *dst = doc->scratch;
    size_t j = 0;
    for (uint32_t i = 0; i < c->len; i++) {
        if (src[i] == '"' && i + 1 < c->len && src[i + 1] == '"') { dst[j++] = '"'; i++; }
        else dst[j++] = src[i];
    }
    dst[j] = '\0';
    if (len) *len = j;
    return dst;
}

size_t hpcs_cell_text_copy(const HpDoc *doc, uint32_t row, uint32_t col, char *dst, size_t cap) {
    const HpCell *c = hpcs_cell_at(doc, row, col);
    if (!c) { if (cap) dst[0] = '\0'; return 0; }
    const char *src = doc->buf + c->off;
    size_t j = 0;
    for (uint32_t i = 0; i < c->len; i++) {
        if (c->quoted && src[i] == '"' && i + 1 < c->len && src[i + 1] == '"') { i++; }
        if (j + 1 < cap) dst[j++] = src[i];
    }
    if (cap) dst[j] = '\0';
    return j;
}

static int cell_needs_quotes(const char *s, size_t n, char delim) {
    if (n == 0) return 0;
    if ((unsigned char)s[0] == ' ' || (unsigned char)s[0] == '\t') return 1;
    if ((unsigned char)s[n - 1] == ' ' || (unsigned char)s[n - 1] == '\t') return 1;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '"' || c == delim || c == '\n' || c == '\r') return 1;
    }
    return 0;
}

static char *encode_cell(const char *s, size_t n, char delim, size_t *out_len,
                         uint32_t *inner_len, int *out_quoted) {
    int quoted = cell_needs_quotes(s, n, delim);
    size_t quotes = 0;
    if (quoted)
        for (size_t i = 0; i < n; i++)
            if (s[i] == '"') quotes++;

    size_t total = quoted ? n + quotes + 2 : n;
    if (total > UINT32_MAX) return NULL;
    char *out = (char *)malloc(total + 1);
    if (!out) return NULL;
    size_t j = 0;
    if (quoted) {
        out[j++] = '"';
        for (size_t i = 0; i < n; i++) {
            if (s[i] == '"') out[j++] = '"';
            out[j++] = s[i];
        }
        out[j++] = '"';
    } else if (n) {
        memcpy(out, s, n);
        j = n;
    }
    out[j] = '\0';
    *out_len = j;
    *inner_len = (uint32_t)(quoted ? n + quotes : n);
    *out_quoted = quoted;
    return out;
}

typedef struct Splice {
    uint32_t cell_idx;
    uint32_t start;
    uint32_t end;
    uint32_t new_start;
    uint32_t inner_len;
    char *data;
    size_t dlen;
    int quoted;
} Splice;

static int splice_cmp(const void *a, const void *b) {
    const Splice *x = (const Splice *)a;
    const Splice *y = (const Splice *)b;
    if (x->start < y->start) return -1;
    if (x->start > y->start) return 1;
    if (x->cell_idx < y->cell_idx) return -1;
    if (x->cell_idx > y->cell_idx) return 1;
    return 0;
}

HpStatus hpcs_set_cells(HpDoc *doc, const HpCellEdit *edits, size_t n) {
    if (n == 0) return HPCS_OK;
    if (!edits) return HPCS_ERR_ARG;
    if (n > UINT32_MAX) return HPCS_ERR_ARG;

    Splice *sp = (Splice *)calloc(n, sizeof(Splice));
    if (!sp) return HPCS_ERR_NOMEM;
    HpStatus status = HPCS_OK;

    for (size_t k = 0; k < n; k++) {
        uint32_t r = edits[k].row, c = edits[k].col;
        if (r >= doc->n_rows || c >= doc->rows[r].count) { status = HPCS_ERR_RANGE; goto done; }
        const HpCell *cell = &doc->cells[doc->rows[r].first + c];
        sp[k].cell_idx = doc->rows[r].first + c;
        sp[k].start = cell->quoted ? cell->off - 1 : cell->off;
        sp[k].end = sp[k].start + cell->raw_len;
        if (edits[k].len && !edits[k].text) { status = HPCS_ERR_ARG; goto done; }
        sp[k].data = encode_cell(edits[k].text ? edits[k].text : "", edits[k].len, doc->delim,
                                 &sp[k].dlen, &sp[k].inner_len, &sp[k].quoted);
        if (!sp[k].data) { status = HPCS_ERR_NOMEM; goto done; }
    }

    qsort(sp, n, sizeof(Splice), splice_cmp);
    for (size_t k = 1; k < n; k++) {
        if (sp[k].cell_idx == sp[k - 1].cell_idx || sp[k].start < sp[k - 1].end) {
            status = HPCS_ERR_ARG;
            goto done;
        }
    }

    size_t new_len = doc->len;
    for (size_t k = 0; k < n; k++) {
        size_t old_span = (size_t)(sp[k].end - sp[k].start);
        if (sp[k].dlen >= old_span) new_len += sp[k].dlen - old_span;
        else new_len -= old_span - sp[k].dlen;
    }
    if (new_len > (size_t)UINT32_MAX - 1) { status = HPCS_ERR_TOO_LARGE; goto done; }

    char *out = (char *)malloc(new_len + 1);
    if (!out) { status = HPCS_ERR_NOMEM; goto done; }

    size_t src = 0, dst = 0;
    for (size_t k = 0; k < n; k++) {
        size_t copy = sp[k].start - src;
        memcpy(out + dst, doc->buf + src, copy);
        dst += copy;
        sp[k].new_start = (uint32_t)dst;
        memcpy(out + dst, sp[k].data, sp[k].dlen);
        dst += sp[k].dlen;
        src = sp[k].end;
    }
    memcpy(out + dst, doc->buf + src, doc->len - src);
    dst += doc->len - src;
    out[dst] = '\0';

    /* Patch offsets. All splices replace whole cells, so cell indices are stable. */
    size_t si = 0;
    int64_t shift = 0;
    for (uint32_t i = 0; i < doc->n_cells; i++) {
        HpCell *cell = &doc->cells[i];
        if (si < n && sp[si].cell_idx == i) {
            cell->off = sp[si].new_start + (uint32_t)(sp[si].quoted ? 1 : 0);
            cell->len = sp[si].inner_len;
            cell->raw_len = (uint32_t)sp[si].dlen;
            cell->quoted = (uint8_t)(sp[si].quoted ? 1 : 0);
            shift += (int64_t)sp[si].dlen - (int64_t)(sp[si].end - sp[si].start);
            si++;
        } else {
            cell->off = (uint32_t)((int64_t)cell->off + shift);
        }
    }

    free(doc->buf);
    doc->buf = out;
    doc->len = dst;
    doc->cap = dst + 1;
    doc->dirty = 1;

done:
    for (size_t k = 0; k < n; k++) free(sp[k].data);
    free(sp);
    return status;
}

HpStatus hpcs_set_cell(HpDoc *doc, uint32_t row, uint32_t col, const char *text, size_t len) {
    HpCellEdit e;
    e.row = row;
    e.col = col;
    e.text = text;
    e.len = len;
    return hpcs_set_cells(doc, &e, 1);
}

HpStatus hpcs_append_row(HpDoc *doc, uint32_t cols) {
    if (cols == 0) cols = 1;
    if (doc->n_cells + cols > UINT32_MAX - 4) return HPCS_ERR_TOO_LARGE;

    size_t prefix = 0;
    int need_nl = 0;
    if (doc->len > 0) {
        char last = doc->buf[doc->len - 1];
        need_nl = (last != '\n' && last != '\r');
    }
    const char *eol = doc->crlf ? "\r\n" : "\n";
    size_t eol_len = doc->crlf ? 2 : 1;
    prefix = (need_nl ? eol_len : 0) + (size_t)(cols - 1) + eol_len;

    char *nb = (char *)malloc(doc->len + prefix + 1);
    if (!nb) return HPCS_ERR_NOMEM;
    memcpy(nb, doc->buf, doc->len);
    size_t p = doc->len;
    if (need_nl) {
        memcpy(nb + p, eol, eol_len);
        p += eol_len;
    }
    for (uint32_t c = 1; c < cols; c++) nb[p++] = doc->delim;
    memcpy(nb + p, eol, eol_len);
    p += eol_len;
    nb[p] = '\0';

    return hpcs_parse(doc, nb, p);
}

void hpcs_column_letter(uint32_t col, char *out, size_t cap) {
    if (!out || cap == 0) return;
    char tmp[16];
    int n = 0;
    uint32_t v = col + 1;
    while (v > 0 && n < 15) {
        uint32_t rem = (v - 1) % 26;
        tmp[n++] = (char)('A' + rem);
        v = (v - 1) / 26;
    }
    size_t j = 0;
    while (n > 0 && j + 1 < cap) out[j++] = tmp[--n];
    out[j] = '\0';
}

const char *hpcs_delimiter_name(char delim, char *out, size_t cap) {
    const char *name = "comma";
    if (delim == ';') name = "semicolon";
    else if (delim == '\t') name = "tab";
    else if (delim == '|') name = "pipe";
    else if (delim != ',') name = "custom";
    if (out && cap) {
        if (delim == '\t') snprintf(out, cap, "%s", name);
        else snprintf(out, cap, "%s (%c)", name, delim);
        return out;
    }
    return name;
}

int hpcs_cell_is_numeric(const char *s, size_t n) {
    size_t i = 0;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    while (n > i && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
    if (i >= n) return 0;
    if (s[i] == '+' || s[i] == '-') i++;
    int digits = 0, dot = 0;
    while (i < n) {
        char c = s[i];
        if (c >= '0' && c <= '9') { digits++; i++; }
        else if (c == '.' && !dot) { dot = 1; i++; }
        else break;
    }
    if (digits == 0) return 0;
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        if (i < n && (s[i] == '+' || s[i] == '-')) i++;
        int ed = 0;
        while (i < n && s[i] >= '0' && s[i] <= '9') { ed++; i++; }
        if (ed == 0) return 0;
    }
    return i == n;
}
