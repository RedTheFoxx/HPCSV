#define _CRT_SECURE_NO_WARNINGS
#include "hpcs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define UNICODE
#  define _UNICODE
#  include <windows.h>
#  include <shellapi.h>
#endif

static char *utf8_from_wide(const wchar_t *w) {
#ifdef _WIN32
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    char *s = (char *)malloc((size_t)n);
    if (!s) return NULL;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
#else
    size_t n = wcslen(w) * 4 + 1;
    char *s = (char *)malloc(n);
    if (!s) return NULL;
    wcstombs(s, w, n);
    return s;
#endif
}

static void usage(void) {
    printf("HPCSV core %s\n\n", hpcs_version());
    printf("usage: hpcs <command> [args]\n\n");
    printf("  info <file>                 document statistics\n");
    printf("  get <file> <row> <col>      print a cell (1-based row/col)\n");
    printf("  set <file> <row> <col> <v>  replace a cell in place\n");
    printf("  append <file> [cols]        append an empty row\n");
    printf("  dump <file> [maxrows]       print cells as TSV\n");
    printf("  bench <file> [iters]        parse benchmark\n");
    printf("  version\n");
}

static const char *base_name(const char *p) {
    const char *s = p;
    for (const char *q = p; *q; q++)
        if (*q == '/' || *q == '\\') s = q + 1;
    return s;
}

static HpDoc load_doc(const char *path) {
    HpDoc d;
    hpcs_init(&d);
    HpStatus st = hpcs_load(&d, path);
    if (st != HPCS_OK) {
        fprintf(stderr, "hpcs: cannot read %s (status %d)\n", path, (int)st);
        exit(2);
    }
    return d;
}

static int cmd_info(int argc, char **argv) {
    if (argc < 3) { usage(); return 2; }
    clock_t t0 = clock();
    HpDoc d = load_doc(argv[2]);
    double ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
    char dbuf[32];
    printf("file       %s\n", base_name(argv[2]));
    printf("bytes      %zu\n", d.len);
    printf("rows       %u\n", d.n_rows);
    printf("columns    %u\n", d.max_cols);
    printf("delimiter  %s\n", hpcs_delimiter_name(d.delim, dbuf, sizeof(dbuf)));
    printf("bom        %s\n", d.bom ? "yes" : "no");
    printf("line ends  %s\n", d.crlf ? "CRLF" : "LF");
    printf("final eol  %s\n", d.final_newline ? "yes" : "no");
    printf("ragged     %u\n", d.ragged_rows);
    printf("warnings   %u\n", d.warnings);
    printf("parsed in  %.2f ms\n", ms);
    hpcs_free(&d);
    return 0;
}

static int cmd_get(int argc, char **argv) {
    if (argc < 5) { usage(); return 2; }
    HpDoc d = load_doc(argv[2]);
    long r = strtol(argv[3], NULL, 10) - 1;
    long c = strtol(argv[4], NULL, 10) - 1;
    if (r < 0 || c < 0 || (uint32_t)r >= d.n_rows || (uint32_t)c >= d.rows[r].count) {
        fprintf(stderr, "hpcs: cell out of range\n");
        hpcs_free(&d);
        return 3;
    }
    size_t n = 0;
    const char *s = hpcs_cell_text(&d, (uint32_t)r, (uint32_t)c, &n);
    fwrite(s, 1, n, stdout);
    fputc('\n', stdout);
    hpcs_free(&d);
    return 0;
}

static int cmd_set(int argc, char **argv) {
    if (argc < 6) { usage(); return 2; }
    HpDoc d = load_doc(argv[2]);
    long r = strtol(argv[3], NULL, 10) - 1;
    long c = strtol(argv[4], NULL, 10) - 1;
    if (r < 0 || c < 0 || (uint32_t)r >= d.n_rows || (uint32_t)c >= d.rows[r].count) {
        fprintf(stderr, "hpcs: cell out of range\n");
        hpcs_free(&d);
        return 3;
    }
    if (hpcs_set_cell(&d, (uint32_t)r, (uint32_t)c, argv[5], strlen(argv[5])) != HPCS_OK) {
        fprintf(stderr, "hpcs: edit failed\n");
        hpcs_free(&d);
        return 4;
    }
    if (hpcs_save(&d, argv[2]) != HPCS_OK) {
        fprintf(stderr, "hpcs: cannot write %s\n", argv[2]);
        hpcs_free(&d);
        return 5;
    }
    hpcs_free(&d);
    return 0;
}

static int cmd_append(int argc, char **argv) {
    if (argc < 3) { usage(); return 2; }
    HpDoc d = load_doc(argv[2]);
    uint32_t cols = argc >= 4 ? (uint32_t)strtoul(argv[3], NULL, 10) : (d.max_cols ? d.max_cols : 1);
    if (hpcs_append_row(&d, cols) != HPCS_OK) {
        hpcs_free(&d);
        return 4;
    }
    int rc = hpcs_save(&d, argv[2]) == HPCS_OK ? 0 : 5;
    hpcs_free(&d);
    return rc;
}

static int cmd_dump(int argc, char **argv) {
    if (argc < 3) { usage(); return 2; }
    HpDoc d = load_doc(argv[2]);
    uint32_t limit = argc >= 4 ? (uint32_t)strtoul(argv[3], NULL, 10) : d.n_rows;
    if (limit > d.n_rows) limit = d.n_rows;
    size_t cap = 1024;
    char *buf = (char *)malloc(cap);
    for (uint32_t r = 0; r < limit; r++) {
        for (uint32_t c = 0; c < d.rows[r].count; c++) {
            size_t need = 0;
            const char *p = hpcs_cell_text(&d, r, c, &need);
            if (need + 2 > cap) {
                cap = need + 2;
                buf = (char *)realloc(buf, cap);
            }
            for (size_t k = 0; k < need; k++) {
                char ch = p[k];
                buf[k] = (ch == '\n' || ch == '\r' || ch == '\t') ? ' ' : ch;
            }
            buf[need] = '\0';
            if (c) fputc('\t', stdout);
            fputs(buf, stdout);
        }
        fputc('\n', stdout);
    }
    free(buf);
    hpcs_free(&d);
    return 0;
}

static int cmd_bench(int argc, char **argv) {
    if (argc < 3) { usage(); return 2; }
    int iters = argc >= 4 ? atoi(argv[3]) : 5;
    if (iters < 1) iters = 1;
    char *data = NULL;
    size_t len = 0;
    FILE *f = fopen(argv[2], "rb");
    if (!f) { fprintf(stderr, "hpcs: cannot open %s\n", argv[2]); return 2; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    data = (char *)malloc((size_t)sz);
    if (fread(data, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(data); return 2; }
    fclose(f);
    len = (size_t)sz;

    double best = 1e30, total = 0;
    uint32_t rows = 0, cols = 0;
    for (int i = 0; i < iters; i++) {
        char *buf = (char *)malloc(len + 1);
        memcpy(buf, data, len);
        buf[len] = '\0';
        HpDoc d;
        hpcs_init(&d);
        clock_t t0 = clock();
        HpStatus st = hpcs_parse(&d, buf, len);
        double ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
        if (st != HPCS_OK) { fprintf(stderr, "parse failed\n"); return 4; }
        if (ms < best) best = ms;
        total += ms;
        rows = d.n_rows;
        cols = d.max_cols;
        hpcs_free(&d);
    }
    double mb = (double)len / (1024.0 * 1024.0);
    printf("%s: %u rows, %u cols, %.2f MiB\n", base_name(argv[2]), rows, cols, mb);
    printf("best %.2f ms  avg %.2f ms  %.1f MiB/s (best)\n", best, total / iters,
           mb / (best / 1000.0));
    free(data);
    return 0;
}

int main(int argc, char **argv) {
#ifdef _WIN32
    int wargc = 0;
    wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    char **uargv = NULL;
    if (wargv) {
        uargv = (char **)calloc((size_t)wargc + 1, sizeof(char *));
        for (int i = 0; i < wargc; i++) uargv[i] = utf8_from_wide(wargv[i]);
        argc = wargc;
        argv = uargv;
    }
#endif
    if (argc < 2) { usage(); return 1; }
    const char *cmd = argv[1];
    int rc;
    if (strcmp(cmd, "info") == 0) rc = cmd_info(argc, argv);
    else if (strcmp(cmd, "get") == 0) rc = cmd_get(argc, argv);
    else if (strcmp(cmd, "set") == 0) rc = cmd_set(argc, argv);
    else if (strcmp(cmd, "append") == 0) rc = cmd_append(argc, argv);
    else if (strcmp(cmd, "dump") == 0) rc = cmd_dump(argc, argv);
    else if (strcmp(cmd, "bench") == 0) rc = cmd_bench(argc, argv);
    else if (strcmp(cmd, "version") == 0 || strcmp(cmd, "--version") == 0) {
        printf("hpcs %s\n", hpcs_version());
        rc = 0;
    } else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0) {
        usage();
        rc = 0;
    } else {
        usage();
        rc = 2;
    }
#ifdef _WIN32
    if (uargv) {
        for (int i = 0; i < argc; i++) free(uargv[i]);
        free(uargv);
        LocalFree(wargv);
    }
#endif
    return rc;
}
