#define _CRT_SECURE_NO_WARNINGS
#include "hpcs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (cond) { g_pass++; }                                                \
        else {                                                                 \
            g_fail++;                                                          \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                      \
    } while (0)

#define CHECK_STR(a, b)                                                        \
    do {                                                                       \
        const char *aa = (a);                                                  \
        const char *bb = (b);                                                  \
        if (aa && bb && strcmp(aa, bb) == 0) { g_pass++; }                     \
        else {                                                                 \
            g_fail++;                                                          \
            fprintf(stderr, "FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__,        \
                    __LINE__, aa ? aa : "(null)", bb ? bb : "(null)");         \
        }                                                                      \
    } while (0)

static HpDoc make(const char *bytes) {
    HpDoc d;
    hpcs_init(&d);
    size_t n = strlen(bytes);
    char *buf = (char *)malloc(n + 1);
    memcpy(buf, bytes, n + 1);
    if (hpcs_parse(&d, buf, n) != HPCS_OK) fprintf(stderr, "parse failed\n");
    return d;
}

static char *cell(HpDoc *d, uint32_t r, uint32_t c) {
    size_t n = 0;
    const char *p = hpcs_cell_text(d, r, c, &n);
    char *out = (char *)malloc(n + 1);
    memcpy(out, p, n);
    out[n] = '\0';
    return out;
}

static void test_basic(void) {
    HpDoc d = make("a,b,c\n1,2,3\n");
    CHECK(hpcs_row_count(&d) == 2);
    CHECK(hpcs_row_cols(&d, 0) == 3);
    CHECK(d.delim == ',');
    char *s = cell(&d, 1, 2);
    CHECK_STR(s, "3");
    free(s);
    s = cell(&d, 0, 0);
    CHECK_STR(s, "a");
    free(s);
    hpcs_free(&d);
}

static void test_quotes(void) {
    HpDoc d = make("\"hello, world\",\"say \"\"hi\"\"\",x\n");
    CHECK(hpcs_row_count(&d) == 1);
    char *s = cell(&d, 0, 0);
    CHECK_STR(s, "hello, world");
    free(s);
    s = cell(&d, 0, 1);
    CHECK_STR(s, "say \"hi\"");
    free(s);
    s = cell(&d, 0, 2);
    CHECK_STR(s, "x");
    free(s);
    size_t raw = 0;
    hpcs_cell_raw(&d, 0, 1, &raw);
    CHECK(raw == 12);
    hpcs_free(&d);
}

static void test_multiline(void) {
    HpDoc d = make("id,note\r\n1,\"line1\r\nline2\"\r\n2,ok\r\n");
    CHECK(hpcs_row_count(&d) == 3);
    CHECK(d.crlf == 1);
    char *s = cell(&d, 1, 1);
    CHECK_STR(s, "line1\r\nline2");
    free(s);
    s = cell(&d, 2, 1);
    CHECK_STR(s, "ok");
    free(s);
    hpcs_free(&d);
}

static void test_bom_and_empty_cells(void) {
    HpDoc d = make("\xEF\xBB\xBF" "a;b;\n;x;y\n");
    CHECK(d.bom == 3);
    CHECK(d.delim == ';');
    CHECK(hpcs_row_cols(&d, 0) == 3);
    char *s = cell(&d, 0, 0);
    CHECK_STR(s, "a");
    free(s);
    s = cell(&d, 0, 2);
    CHECK_STR(s, "");
    free(s);
    s = cell(&d, 1, 0);
    CHECK_STR(s, "");
    free(s);
    hpcs_free(&d);
}

static void test_tab_delim(void) {
    HpDoc d = make("a\tb\tc\n1\t2\t3\n");
    CHECK(d.delim == '\t');
    CHECK(hpcs_row_cols(&d, 0) == 3);
    hpcs_free(&d);
}

static void test_trailing(void) {
    HpDoc d = make("a,b\n1,2");
    CHECK(hpcs_row_count(&d) == 2);
    CHECK(d.final_newline == 0);
    hpcs_free(&d);

    HpDoc e = make("a,b\n1,2,\n");
    CHECK(hpcs_row_cols(&e, 1) == 3);
    char *s = cell(&e, 1, 2);
    CHECK_STR(s, "");
    free(s);
    hpcs_free(&e);

    HpDoc z = make("");
    CHECK(hpcs_row_count(&z) == 0);
    hpcs_append_row(&z, 3);
    CHECK(hpcs_row_count(&z) == 1);
    CHECK(hpcs_row_cols(&z, 0) == 3);
    hpcs_free(&z);
}

static void test_unterminated(void) {
    HpDoc d = make("a,\"oops\n");
    CHECK(d.unterminated == 1);
    char *s = cell(&d, 0, 1);
    CHECK_STR(s, "oops\n");
    free(s);
    hpcs_free(&d);
}

static void test_edit(void) {
    HpDoc d = make("a,b,c\n1,2,3\nx,y,z\n");
    CHECK(hpcs_set_cell(&d, 1, 1, "hello, \"world\"", 14) == HPCS_OK);
    char *s = cell(&d, 1, 1);
    CHECK_STR(s, "hello, \"world\"");
    free(s);
    s = cell(&d, 1, 2);
    CHECK_STR(s, "3");
    free(s);
    s = cell(&d, 2, 0);
    CHECK_STR(s, "x");
    free(s);
    CHECK(d.dirty == 1);

    /* Full reparse of the edited buffer must agree. */
    char *copy = (char *)malloc(d.len + 1);
    memcpy(copy, d.buf, d.len + 1);
    HpDoc e;
    hpcs_init(&e);
    CHECK(hpcs_parse(&e, copy, d.len) == HPCS_OK);
    char *t = cell(&e, 1, 1);
    CHECK_STR(t, "hello, \"world\"");
    free(t);
    CHECK(hpcs_row_count(&e) == 3);
    hpcs_free(&e);
    hpcs_free(&d);
}

static void test_batch_edit(void) {
    HpDoc d = make("a,b,c\nd,e,f\n");
    HpCellEdit edits[3];
    edits[0].row = 0; edits[0].col = 0; edits[0].text = "1"; edits[0].len = 1;
    edits[1].row = 1; edits[1].col = 1; edits[1].text = "two;x"; edits[1].len = 5;
    edits[2].row = 1; edits[2].col = 2; edits[2].text = "end"; edits[2].len = 3;
    CHECK(hpcs_set_cells(&d, edits, 3) == HPCS_OK);
    char *s = cell(&d, 0, 0);
    CHECK_STR(s, "1");
    free(s);
    s = cell(&d, 1, 1);
    CHECK_STR(s, "two;x");
    free(s);
    s = cell(&d, 1, 2);
    CHECK_STR(s, "end");
    free(s);
    hpcs_free(&d);

    HpDoc e = make("a,b\n");
    HpCellEdit bad;
    bad.row = 5; bad.col = 0; bad.text = "x"; bad.len = 1;
    CHECK(hpcs_set_cells(&e, &bad, 1) == HPCS_ERR_RANGE);
    CHECK(hpcs_row_count(&e) == 1);
    hpcs_free(&e);
}

static void test_save_load(void) {
    const char *path = "hpcs-test-tmp.csv";
    HpDoc d = make("name,value\n\"a,b\",2\n");
    CHECK(hpcs_set_cell(&d, 1, 1, "42", 2) == HPCS_OK);
    CHECK(hpcs_save(&d, path) == HPCS_OK);
    hpcs_free(&d);

    HpDoc e;
    hpcs_init(&e);
    CHECK(hpcs_load(&e, path) == HPCS_OK);
    CHECK(hpcs_row_count(&e) == 2);
    char *s = cell(&e, 1, 0);
    CHECK_STR(s, "a,b");
    free(s);
    s = cell(&e, 1, 1);
    CHECK_STR(s, "42");
    free(s);
    hpcs_free(&e);
    remove(path);
}

static void test_perf_and_offsets(void) {
    const uint32_t rows = 50000, cols = 10;
    size_t cap = (size_t)rows * cols * 24 + 64;
    char *buf = (char *)malloc(cap);
    size_t p = 0;
    for (uint32_t r = 0; r < rows; r++) {
        for (uint32_t c = 0; c < cols; c++) {
            p += (size_t)snprintf(buf + p, cap - p, "r%u_c%u", r, c);
            if (c + 1 < cols) buf[p++] = ',';
        }
        buf[p++] = '\n';
    }
    buf[p] = '\0';

    HpDoc d;
    hpcs_init(&d);
    clock_t t0 = clock();
    CHECK(hpcs_parse(&d, buf, p) == HPCS_OK);
    double ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
    CHECK(hpcs_row_count(&d) == rows);
    printf("  parsed %u rows x %u cols (%.1f MiB) in %.1f ms\n", rows, cols,
           (double)p / (1024.0 * 1024.0), ms);

    char before[64], after[64];
    size_t n = 0;
    const char *s = hpcs_cell_text(&d, rows - 1, cols - 1, &n);
    snprintf(before, sizeof(before), "%.*s", (int)n, s);

    CHECK(hpcs_set_cell(&d, 0, 0, "first cell changed", 18) == HPCS_OK);
    s = hpcs_cell_text(&d, rows - 1, cols - 1, &n);
    snprintf(after, sizeof(after), "%.*s", (int)n, s);
    CHECK_STR(before, after);

    s = hpcs_cell_text(&d, 0, 0, &n);
    CHECK(n == 18 && memcmp(s, "first cell changed", 18) == 0);
    s = hpcs_cell_text(&d, 0, 1, &n);
    CHECK(n == 5 && memcmp(s, "r0_c1", 5) == 0);

    hpcs_free(&d);
}

static void test_misc(void) {
    char name[8];
    hpcs_column_letter(0, name, sizeof(name));
    CHECK_STR(name, "A");
    hpcs_column_letter(25, name, sizeof(name));
    CHECK_STR(name, "Z");
    hpcs_column_letter(26, name, sizeof(name));
    CHECK_STR(name, "AA");
    hpcs_column_letter(701, name, sizeof(name));
    CHECK_STR(name, "ZZ");
    hpcs_column_letter(702, name, sizeof(name));
    CHECK_STR(name, "AAA");

    CHECK(hpcs_cell_is_numeric("-12.5e3", 7) == 1);
    CHECK(hpcs_cell_is_numeric("+7", 2) == 1);
    CHECK(hpcs_cell_is_numeric("1,2", 3) == 0);
    CHECK(hpcs_cell_is_numeric("abc", 3) == 0);
    CHECK(hpcs_cell_is_numeric(" 42 ", 4) == 1);
}

int main(void) {
    printf("hpcs %s - core tests\n", hpcs_version());
    test_basic();
    test_quotes();
    test_multiline();
    test_bom_and_empty_cells();
    test_tab_delim();
    test_trailing();
    test_unterminated();
    test_edit();
    test_batch_edit();
    test_save_load();
    test_perf_and_offsets();
    test_misc();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
