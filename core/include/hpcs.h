/*
 * HPCSV core - high performance CSV engine.
 *
 * Pure C23, no dependencies. Zero-copy parsing: every cell keeps byte
 * offsets into the document buffer. Editing rewrites only the affected
 * byte ranges and patches every following offset, so a document of
 * hundreds of megabytes can be edited without a full reparse.
 */
#ifndef HPCS_H
#define HPCS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum HpStatus {
    HPCS_OK = 0,
    HPCS_ERR_NOMEM,
    HPCS_ERR_IO,
    HPCS_ERR_TOO_LARGE,
    HPCS_ERR_RANGE,
    HPCS_ERR_ARG,
    HPCS_ERR_CORRUPT
} HpStatus;

typedef struct HpCell {
    uint32_t off;     /* content start (inside the quotes when quoted) */
    uint32_t len;     /* content length, doubled quotes still escaped */
    uint32_t raw_len; /* full span, quotes included */
    uint8_t quoted;
    uint8_t pad[3];
} HpCell;

typedef struct HpRow {
    uint32_t first; /* index of the first cell */
    uint32_t count;
} HpRow;

typedef struct HpDoc {
    char *buf; /* file bytes, NUL terminated */
    size_t len;
    size_t cap;

    HpCell *cells;
    uint32_t n_cells;
    uint32_t cap_cells;

    HpRow *rows;
    uint32_t n_rows;
    uint32_t cap_rows;

    char delim;
    uint8_t bom;
    uint8_t crlf;
    uint8_t final_newline;
    uint8_t dirty;
    uint32_t max_cols;
    uint32_t ragged_rows;
    uint32_t unterminated;
    uint32_t first_unterminated;
    uint32_t warnings;

    char *scratch;
    size_t scratch_cap;
} HpDoc;

typedef struct HpCellEdit {
    uint32_t row;
    uint32_t col;
    const char *text;
    size_t len;
} HpCellEdit;

void hpcs_init(HpDoc *doc);
void hpcs_free(HpDoc *doc);

/* Takes ownership of buf (allocated with malloc). */
HpStatus hpcs_parse(HpDoc *doc, char *buf, size_t len);
HpStatus hpcs_load(HpDoc *doc, const char *utf8_path);
HpStatus hpcs_save(HpDoc *doc, const char *utf8_path);

uint32_t hpcs_row_count(const HpDoc *doc);
uint32_t hpcs_row_cols(const HpDoc *doc, uint32_t row);

const HpCell *hpcs_cell_at(const HpDoc *doc, uint32_t row, uint32_t col);

/* Raw bytes of the cell, exactly as stored in the file. */
const char *hpcs_cell_raw(const HpDoc *doc, uint32_t row, uint32_t col, size_t *len);

/* Decoded text (doubled quotes collapsed). The returned pointer may point
 * into the document or into the document scratch buffer. */
const char *hpcs_cell_text(HpDoc *doc, uint32_t row, uint32_t col, size_t *len);
size_t hpcs_cell_text_copy(const HpDoc *doc, uint32_t row, uint32_t col, char *dst, size_t cap);

HpStatus hpcs_set_cell(HpDoc *doc, uint32_t row, uint32_t col, const char *text, size_t len);
HpStatus hpcs_set_cells(HpDoc *doc, const HpCellEdit *edits, size_t n);

/* Appends a row of `cols` empty cells. Structural, triggers a reparse. */
HpStatus hpcs_append_row(HpDoc *doc, uint32_t cols);

/* "A", "B", ... "Z", "AA", ... */
void hpcs_column_letter(uint32_t col, char *out, size_t cap);
const char *hpcs_delimiter_name(char delim, char *out, size_t cap);
int hpcs_cell_is_numeric(const char *s, size_t len);
const char *hpcs_version(void);

#ifdef __cplusplus
}
#endif

#endif /* HPCS_H */
