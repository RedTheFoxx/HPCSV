#ifndef HPCS_APP_H
#define HPCS_APP_H

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <commdlg.h>

#include "hpcs.h"

#define HPCS_CLASS L"HPCSV.MainWindow"
#define HPCS_TITLE L"HPCSV"

#define C_BG        RGB(30, 30, 30)
#define C_CELL      RGB(30, 30, 30)
#define C_ALT       RGB(37, 37, 38)
#define C_HEADER    RGB(45, 45, 48)
#define C_HEADER_HI RGB(54, 54, 58)
#define C_GRID      RGB(58, 58, 60)
#define C_TEXT      RGB(214, 214, 214)
#define C_DIM       RGB(138, 138, 141)
#define C_SEL       RGB(9, 71, 113)
#define C_SEL_TEXT  RGB(255, 255, 255)
#define C_SEL_EDGE  RGB(0, 122, 204)
#define C_ACCENT    RGB(0, 122, 204)
#define C_BAD       RGB(219, 108, 108)
#define C_OK        RGB(106, 190, 120)
#define C_WARN      RGB(220, 175, 90)

typedef struct UndoCell {
    uint32_t row, col;
    char *old_text;
    size_t old_len;
    char *new_text;
    size_t new_len;
} UndoCell;

typedef struct UndoBatch {
    UndoCell *cells;
    size_t n;
} UndoBatch;

typedef struct App {
    HWND hwnd;
    HpDoc doc;

    wchar_t *path;
    char *path_u8;
    wchar_t file_name[260];

    uint64_t last_write;
    uint64_t last_size;
    int external_change;

    int dpi;
    int row_h, header_h, gutter_w, status_h;
    HFONT font, font_bold, font_small;
    int font_h;

    int *colw;
    int *colx;
    uint32_t ncols;
    uint32_t ncols_alloc;
    int scroll_x, scroll_y;

    int sel_row, sel_col;
    int anchor_row, anchor_col;
    int dragging;
    int resizing_col;
    int resize_x, resize_w;

    HWND edit;
    int editing;
    int edit_row, edit_col;

    UndoBatch undo[64];
    int undo_pos;

    int dirty_save;
    UINT_PTR save_timer;
    UINT_PTR watch_timer;

    wchar_t status[512];
    int status_kind;
    wchar_t hint[256];

    HDC mem_dc;
    HBITMAP mem_bmp;
    int mem_w, mem_h;
} App;

/* main.c */
void app_set_status(App *a, int kind, const wchar_t *fmt, ...);
void app_close_file(App *a);
int app_open_file(App *a, const wchar_t *path);
void app_apply_edits(App *a, HpCellEdit *edits, size_t n);
void app_invalidate(App *a);
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 1, 2)))
#endif
void app_dbg(const char *fmt, ...);

/* render.c */
void app_metrics(App *a);
void app_ensure_cols(App *a);
int app_colx(App *a, uint32_t col);
int app_col_at(App *a, int x);
void app_autofit_all(App *a);
void app_autofit_one(App *a, uint32_t col);
int app_view_w(App *a);
int app_view_h(App *a);
void app_cell_rect(App *a, int row, int col, RECT *out);
void app_paint(App *a, HDC hdc, int width, int height);
void app_scrollbars(App *a);

#endif
