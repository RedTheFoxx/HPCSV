#define _CRT_SECURE_NO_WARNINGS
#include "hpcs_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static wchar_t *g_wbuf = NULL;
static size_t g_wcap = 0;

static const wchar_t *cell_wtext(const char *s, size_t n) {
    if (n + 1 > g_wcap) {
        size_t cap = n + 64;
        wchar_t *p = (wchar_t *)realloc(g_wbuf, cap * sizeof(wchar_t));
        if (!p) return L"";
        g_wbuf = p;
        g_wcap = cap;
    }
    int wlen = MultiByteToWideChar(CP_UTF8, 0, s, (int)(n > 0x7fffffff ? 0x7fffffff : n),
                                   g_wbuf, (int)g_wcap - 1);
    if (wlen <= 0) {
        wlen = MultiByteToWideChar(CP_ACP, 0, s, (int)(n > 0x7fffffff ? 0x7fffffff : n),
                                   g_wbuf, (int)g_wcap - 1);
        if (wlen <= 0) wlen = 0;
    }
    g_wbuf[wlen] = 0;
    for (int i = 0; i < wlen; i++) {
        wchar_t ch = g_wbuf[i];
        if (ch == L'\n' || ch == L'\r' || ch == L'\t' || ch < 32) g_wbuf[i] = L' ';
    }
    return g_wbuf;
}

void app_metrics(App *a) {
    if (a->dpi <= 0) a->dpi = 96;
    a->row_h = MulDiv(24, a->dpi, 96);
    a->header_h = MulDiv(30, a->dpi, 96);
    a->gutter_w = MulDiv(56, a->dpi, 96);
    a->status_h = MulDiv(26, a->dpi, 96);
}

static int default_colw(App *a) { return MulDiv(150, a->dpi, 96); }

void app_ensure_cols(App *a) {
    uint32_t n = a->doc.max_cols;
    if (n != a->ncols_alloc) {
        int *w = (int *)realloc(a->colw, (size_t)n * sizeof(int));
        int *x = (int *)realloc(a->colx, ((size_t)n + 1) * sizeof(int));
        if (!w || !x) {
            free(w ? w : a->colw);
            free(x ? x : a->colx);
            a->colw = NULL; a->colx = NULL;
            a->ncols = a->ncols_alloc = 0;
            return;
        }
        a->colw = w;
        a->colx = x;
        if (n > a->ncols_alloc) {
            int d = default_colw(a);
            for (uint32_t i = a->ncols_alloc; i < n; i++) a->colw[i] = d;
        }
        a->ncols_alloc = n;
    }
    a->ncols = n;
    if (a->colx && a->colw) {
        int acc = 0;
        a->colx[0] = 0;
        for (uint32_t i = 0; i < n; i++) {
            acc += a->colw[i];
            a->colx[i + 1] = acc;
        }
    }
}

int app_colx(App *a, uint32_t col) {
    if (!a->colx || col > a->ncols) return 0;
    return a->colx[col];
}

int app_col_at(App *a, int x) {
    if (!a->colx || a->ncols == 0) return -1;
    int cx = x - a->gutter_w + a->scroll_x;
    if (cx < 0) return 0;
    /* largest col with colx[col] <= cx */
    uint32_t lo = 0, hi = a->ncols;
    while (lo + 1 < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (a->colx[mid] <= cx) lo = mid;
        else hi = mid;
    }
    return (int)lo;
}

int app_view_w(App *a) {
    RECT rc;
    GetClientRect(a->hwnd, &rc);
    int w = rc.right - a->gutter_w;
    return w < 0 ? 0 : w;
}

int app_view_h(App *a) {
    RECT rc;
    GetClientRect(a->hwnd, &rc);
    int h = rc.bottom - a->header_h - a->status_h;
    return h < 0 ? 0 : h;
}

void app_scrollbars(App *a) {
    if (!a->hwnd) return;
    RECT rc;
    GetClientRect(a->hwnd, &rc);
    int vw = app_view_w(a), vh = app_view_h(a);
    int total_w = (a->ncols && a->colx) ? a->colx[a->ncols] : 0;
    int total_h = a->doc.n_rows > 1 ? (int)(a->doc.n_rows - 1) * a->row_h : 0;

    int maxx = total_w - vw; if (maxx < 0) maxx = 0;
    int maxy = total_h - vh; if (maxy < 0) maxy = 0;
    if (a->scroll_x < 0) a->scroll_x = 0;
    if (a->scroll_x > maxx) a->scroll_x = maxx;
    if (a->scroll_y < 0) a->scroll_y = 0;
    if (a->scroll_y > maxy) a->scroll_y = maxy;

    SCROLLINFO si;
    memset(&si, 0, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = total_h > 0 ? total_h - 1 : 0;
    si.nPage = (UINT)vh;
    si.nPos = a->scroll_y;
    SetScrollInfo(a->hwnd, SB_VERT, &si, TRUE);

    memset(&si, 0, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin = 0;
    si.nMax = total_w > 0 ? total_w - 1 : 0;
    si.nPage = (UINT)vw;
    si.nPos = a->scroll_x;
    SetScrollInfo(a->hwnd, SB_HORZ, &si, TRUE);
}

void app_cell_rect(App *a, int row, int col, RECT *out) {
    int x = a->gutter_w + app_colx(a, (uint32_t)col) - a->scroll_x;
    int w = (col >= 0 && col < (int)a->ncols) ? a->colw[col] : 100;
    out->left = x;
    out->right = x + w;
    if (row == 0) {
        out->top = 0;
        out->bottom = a->header_h;
    } else {
        out->top = a->header_h + (row - 1) * a->row_h - a->scroll_y;
        out->bottom = out->top + a->row_h;
    }
}

static HDC measure_dc(App *a) {
    HDC dc = GetDC(a->hwnd);
    SelectObject(dc, a->font);
    SetBkMode(dc, TRANSPARENT);
    return dc;
}

static void autofit_range(App *a, uint32_t c0, uint32_t c1) {
    if (a->ncols == 0 || !a->colw) return;
    HDC dc = measure_dc(a);
    int maxw = MulDiv(72, a->dpi, 96);
    int hardcap = MulDiv(460, a->dpi, 96);

    char letter[8];
    hpcs_column_letter(c0, letter, 8);
    wchar_t wletter[8];
    MultiByteToWideChar(CP_UTF8, 0, letter, -1, wletter, 8);
    SIZE sz;
    GetTextExtentPoint32W(dc, wletter, (int)wcslen(wletter), &sz);
    if (sz.cx + MulDiv(20, a->dpi, 96) > maxw) maxw = sz.cx + MulDiv(20, a->dpi, 96);

    uint32_t rows = a->doc.n_rows;
    if (rows > 61) rows = 61;
    for (uint32_t r = 0; r < rows; r++) {
        if (c0 >= a->doc.rows[r].count) continue;
        size_t n = 0;
        const char *t = hpcs_cell_text(&a->doc, r, c0, &n);
        if (n == 0) continue;
        const wchar_t *w = cell_wtext(t, n);
        int wl = (int)wcslen(w);
        if (wl > 200) wl = 200;
        GetTextExtentPoint32W(dc, w, wl, &sz);
        if (sz.cx + MulDiv(20, a->dpi, 96) > maxw) maxw = sz.cx + MulDiv(20, a->dpi, 96);
        if (maxw >= hardcap) break;
    }
    if (maxw > hardcap) maxw = hardcap;
    for (uint32_t c = c0; c <= c1 && c < a->ncols; c++) a->colw[c] = maxw;
    ReleaseDC(a->hwnd, dc);
}

void app_autofit_one(App *a, uint32_t col) { autofit_range(a, col, col); app_ensure_cols(a); }

void app_autofit_all(App *a) {
    app_ensure_cols(a);
    if (a->ncols == 0) return;
    HDC dc = measure_dc(a);
    ReleaseDC(a->hwnd, dc);
    uint32_t limit = a->ncols < 512 ? a->ncols : 512;
    for (uint32_t c = 0; c < limit; c++) autofit_range(a, c, c);
    app_ensure_cols(a);
}

/* --------------------------------------------------------------- painting */

static void ensure_backbuffer(App *a, HDC ref, int w, int h) {
    if (a->mem_dc && a->mem_w == w && a->mem_h == h) return;
    if (a->mem_bmp) { DeleteObject(a->mem_bmp); a->mem_bmp = NULL; }
    if (a->mem_dc) { DeleteDC(a->mem_dc); a->mem_dc = NULL; }
    a->mem_dc = CreateCompatibleDC(ref);
    a->mem_bmp = CreateCompatibleBitmap(ref, w, h);
    if (a->mem_dc && a->mem_bmp) SelectObject(a->mem_dc, a->mem_bmp);
    a->mem_w = w;
    a->mem_h = h;
}

static void fill(HDC dc, int l, int t, int r, int b, COLORREF c) {
    RECT rc = { l, t, r, b };
    HBRUSH br = CreateSolidBrush(c);
    FillRect(dc, &rc, br);
    DeleteObject(br);
}

static void hline(HDC dc, int x0, int x1, int y, COLORREF c) {
    fill(dc, x0, y, x1, y + 1, c);
}

static void vline(HDC dc, int x, int y0, int y1, COLORREF c) {
    fill(dc, x, y0, x + 1, y1, c);
}

static void draw_cell_text(App *a, HDC dc, const char *s, size_t n, RECT *rc, int align_right,
                           int bold) {
    const wchar_t *w = cell_wtext(s, n);
    HFONT old = (HFONT)SelectObject(dc, bold ? a->font_bold : a->font);
    RECT r = *rc;
    r.left += MulDiv(7, a->dpi, 96);
    r.right -= MulDiv(5, a->dpi, 96);
    UINT fmt = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX;
    if (align_right) fmt |= DT_RIGHT;
    DrawTextW(dc, w, -1, &r, fmt);
    SelectObject(dc, old);
}

static void draw_status(App *a, HDC dc, int width, int height) {
    int top = height - a->status_h;
    fill(dc, 0, top, width, height, C_HEADER);
    hline(dc, 0, width, top, C_GRID);

    COLORREF c = C_DIM;
    if (a->status_kind == 1) c = C_ACCENT;
    else if (a->status_kind == 2) c = C_WARN;
    else if (a->status_kind == 3) c = C_BAD;

    HFONT old = (HFONT)SelectObject(dc, a->font_small);
    SetTextColor(dc, c);
    RECT r = { MulDiv(10, a->dpi, 96), top, width - MulDiv(10, a->dpi, 96), height };
    r.top += 4;
    DrawTextW(dc, a->status, -1, &r, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    const wchar_t *hint = L"Enter edit  \u00b7  Ctrl+S save  \u00b7  Ctrl+Z undo  \u00b7  Ctrl+C/V  \u00b7  Ctrl+R reload";
    SetTextColor(dc, RGB(110, 110, 115));
    r = (RECT){ 0, top, width - MulDiv(12, a->dpi, 96), height };
    r.top += 4;
    DrawTextW(dc, hint, -1, &r, DT_SINGLELINE | DT_RIGHT | DT_NOPREFIX);
    SelectObject(dc, old);
}

void app_paint(App *a, HDC hdc, int width, int height) {
    if (width <= 0 || height <= 0) return;
    ensure_backbuffer(a, hdc, width, height);
    HDC dc = a->mem_dc ? a->mem_dc : hdc;

    fill(dc, 0, 0, width, height, C_BG);
    SetBkMode(dc, TRANSPARENT);

    int vw = width - a->gutter_w;
    int vh = height - a->header_h - a->status_h;
    if (vw < 0) vw = 0;
    if (vh < 0) vh = 0;

    int r0, r1, c0, c1;
    int ar = a->anchor_row, as = a->anchor_col, sr = a->sel_row, sc = a->sel_col;
    r0 = ar < sr ? ar : sr;
    r1 = ar > sr ? ar : sr;
    c0 = as < sc ? as : sc;
    c1 = as > sc ? as : sc;
    app_dbg("paint2: sel=(%d,%d) anchor=(%d,%d) bounds r=%d..%d c=%d..%d ncols=%u",
            a->sel_row, a->sel_col, a->anchor_row, a->anchor_col, r0, r1, c0, c1, a->ncols);

    /* visible columns */
    uint32_t first_col = 0;
    if (a->ncols > 0) {
        first_col = (uint32_t)app_col_at(a, a->gutter_w);
        if (first_col >= a->ncols) first_col = 0;
    }
    uint32_t last_col = first_col;
    if (a->ncols > 0) {
        while (last_col + 1 < a->ncols &&
               a->gutter_w + app_colx(a, last_col + 1) - a->scroll_x < width)
            last_col++;
    }

    /* body rows */
    int first_body = 1;
    if (a->doc.n_rows > 1) {
        first_body = 1 + a->scroll_y / a->row_h;
        if (first_body >= (int)a->doc.n_rows) first_body = (int)a->doc.n_rows - 1;
    }

    int body_top = a->header_h;
    int body_bottom = height - a->status_h;

    /* clip body */
    HRGN clip = CreateRectRgn(a->gutter_w, body_top, width, body_bottom);
    SelectClipRgn(dc, clip);
    DeleteObject(clip);

    /* row gutter background + numbers */
    fill(dc, 0, body_top, a->gutter_w, body_bottom, C_HEADER);

    int y = body_top - (a->scroll_y % a->row_h);
    for (int r = first_body; r < (int)a->doc.n_rows; r++) {
        if (y >= body_bottom) break;
        int row_sel = (r >= r0 && r <= r1);
        if (row_sel) fill(dc, 0, y, a->gutter_w, y + a->row_h, C_SEL);
        wchar_t num[24];
        _snwprintf(num, 23, L"%d", r + 1);
        num[23] = 0;
        HFONT old = (HFONT)SelectObject(dc, a->font_small);
        SetTextColor(dc, row_sel ? C_SEL_TEXT : C_DIM);
        RECT nr = { 0, y, a->gutter_w - 8, y + a->row_h };
        DrawTextW(dc, num, -1, &nr, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
        if (a->doc.ragged_rows && a->doc.n_rows > 0 &&
            a->doc.rows[r].count != a->doc.rows[0].count) {
            fill(dc, 0, y + 4, 3, y + a->row_h - 4, C_WARN);
        }
        SelectObject(dc, old);
        vline(dc, a->gutter_w - 1, y, y + a->row_h, C_GRID);
        hline(dc, 0, a->gutter_w, y + a->row_h - 1, C_GRID);
        y += a->row_h;
    }

    /* cells */
    y = body_top - (a->scroll_y % a->row_h);
    for (int r = first_body; r < (int)a->doc.n_rows; r++) {
        if (y >= body_bottom) break;
        int ry = y;
        uint32_t cols = a->doc.rows[r].count;
        for (uint32_t c = first_col; c <= last_col && c < cols; c++) {
            int x = a->gutter_w + app_colx(a, c) - a->scroll_x;
            int w = a->colw[c];
            if (x + w <= a->gutter_w) continue;
            if (x >= width) break;
            int selected = (r >= r0 && r <= r1 && (int)c >= c0 && (int)c <= c1);
            COLORREF bg = selected ? C_SEL : ((r & 1) ? C_ALT : C_CELL);
            fill(dc, x, ry, x + w, ry + a->row_h, bg);

            size_t n = 0;
            const char *t = hpcs_cell_text(&a->doc, (uint32_t)r, c, &n);
            int numeric = hpcs_cell_is_numeric(t, n);
            SetTextColor(dc, selected ? C_SEL_TEXT : (numeric ? RGB(190, 220, 190) : C_TEXT));
            RECT cr = { x, ry, x + w, ry + a->row_h };
            draw_cell_text(a, dc, t, n, &cr, numeric, 0);
            vline(dc, x + w - 1, ry, ry + a->row_h, C_GRID);
        }
        hline(dc, a->gutter_w, width, ry + a->row_h - 1, C_GRID);
        y += a->row_h;
    }

    /* selection outline on current cell */
    if (!a->editing && a->doc.n_rows > 0 && a->sel_row < (int)a->doc.n_rows) {
        RECT cr;
        app_cell_rect(a, a->sel_row, a->sel_col, &cr);
        if (cr.top < body_bottom && cr.bottom > body_top) {
            HPEN pen = CreatePen(PS_SOLID, 2, C_SEL_EDGE);
            HGDIOBJ oldp = SelectObject(dc, pen);
            HGDIOBJ oldb = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, cr.left + 1, cr.top + 1, cr.right - 1, cr.bottom - 1);
            SelectObject(dc, oldp);
            SelectObject(dc, oldb);
            DeleteObject(pen);
        }
    }

    /* header (pinned) */
    SelectClipRgn(dc, NULL);
    fill(dc, 0, 0, width, a->header_h, C_HEADER);
    fill(dc, 0, 0, a->gutter_w, a->header_h, RGB(38, 38, 40));

    HFONT oldf = (HFONT)SelectObject(dc, a->font_small);
    SetTextColor(dc, RGB(120, 120, 125));
    RECT corner = { 0, 0, a->gutter_w - 6, a->header_h };
    DrawTextW(dc, L"CSV", -1, &corner, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
    SelectObject(dc, oldf);

    if (a->ncols > 0) {
        for (uint32_t c = first_col; c <= last_col && c < a->ncols; c++) {
            int x = a->gutter_w + app_colx(a, c) - a->scroll_x;
            int w = a->colw[c];
            if (x >= width) break;
            int selected_col = (a->doc.n_rows > 0 && (int)c >= c0 && (int)c <= c1);
            fill(dc, x, 0, x + w, a->header_h, selected_col ? C_HEADER_HI : C_HEADER);
            vline(dc, x + w - 1, 0, a->header_h, C_GRID);

            size_t n = 0;
            const char *t = "";
            if (a->doc.n_rows > 0 && c < a->doc.rows[0].count)
                t = hpcs_cell_text(&a->doc, 0, c, &n);
            char letter[8];
            if (n == 0) {
                hpcs_column_letter(c, letter, sizeof(letter));
                SetTextColor(dc, selected_col ? C_SEL_TEXT : RGB(180, 180, 185));
                RECT cr = { x, 0, x + w, a->header_h };
                draw_cell_text(a, dc, letter, strlen(letter), &cr, 0, 1);
            } else {
                SetTextColor(dc, selected_col ? C_SEL_TEXT : RGB(235, 235, 238));
                RECT cr = { x, 0, x + w, a->header_h };
                draw_cell_text(a, dc, t, n, &cr, 0, 1);
            }
        }
    }
    hline(dc, 0, width, a->header_h - 1, RGB(70, 70, 74));

    /* status */
    draw_status(a, dc, width, height);

    if (a->doc.n_rows == 0) {
        SetTextColor(dc, C_DIM);
        HFONT old = (HFONT)SelectObject(dc, a->font);
        RECT r = { 0, a->header_h, width, height - a->status_h };
        DrawTextW(dc, L"Empty document \u2014 drag a .csv file here or press Ctrl+O",
                  -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, old);
    }

    if (dc != hdc) BitBlt(hdc, 0, 0, width, height, dc, 0, 0, SRCCOPY);
}
