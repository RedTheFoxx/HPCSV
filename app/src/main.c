#define _CRT_SECURE_NO_WARNINGS
#include "hpcs_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <wchar.h>

App g_app;

static WNDPROC g_edit_proc = NULL;

/* ------------------------------------------------------------------ utils */

static wchar_t *utf8_to_wide(const char *s) {
    if (!s) return NULL;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) n = MultiByteToWideChar(CP_ACP, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!w) return NULL;
    if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n))
        MultiByteToWideChar(CP_ACP, 0, s, -1, w, n);
    return w;
}

static char *wide_to_utf8(const wchar_t *w) {
    if (!w) return NULL;
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    char *s = (char *)malloc((size_t)n);
    if (!s) return NULL;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

static wchar_t *utf8n_to_wide(const char *s, size_t n) {
    if (!s) return NULL;
    if (n > 0x7ffffff0) n = 0x7ffffff0;
    int cnt = MultiByteToWideChar(CP_UTF8, 0, s, (int)n, NULL, 0);
    UINT cp = CP_UTF8;
    if (cnt <= 0) { cp = CP_ACP; cnt = MultiByteToWideChar(cp, 0, s, (int)n, NULL, 0); }
    if (cnt <= 0) return NULL;
    wchar_t *w = (wchar_t *)malloc(((size_t)cnt + 1) * sizeof(wchar_t));
    if (!w) return NULL;
    MultiByteToWideChar(cp, 0, s, (int)n, w, cnt);
    w[cnt] = 0;
    return w;
}

static void file_stamp(App *a, uint64_t *write_time, uint64_t *size) {
    *write_time = 0;
    *size = 0;
    if (!a->path) return;
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(a->path, GetFileExInfoStandard, &fad)) return;
    *write_time = ((uint64_t)fad.ftLastWriteTime.dwHighDateTime << 32) |
                  fad.ftLastWriteTime.dwLowDateTime;
    *size = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
}

static void dbg_log(const char *fmt, ...) {
    if (!getenv("HPCS_DEBUG")) return;
    FILE *f = fopen("C:\\Users\\Red\\AppData\\Local\\Temp\\opencode\\hpcs.log", "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

void app_dbg(const char *fmt, ...) {
    if (!getenv("HPCS_DEBUG")) return;
    FILE *f = fopen("C:\\Users\\Red\\AppData\\Local\\Temp\\opencode\\hpcs.log", "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

void app_set_status(App *a, int kind, const wchar_t *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(a->status, 511, fmt, ap);
    va_end(ap);
    a->status[511] = L'\0';
    a->status_kind = kind;
    if (a->hwnd) InvalidateRect(a->hwnd, NULL, FALSE);
}

void app_invalidate(App *a) {
    if (a->hwnd) InvalidateRect(a->hwnd, NULL, FALSE);
}

static void update_title(App *a) {
    if (!a->hwnd) return;
    wchar_t title[512];
    if (a->path) _snwprintf(title, 511, L"HPCSV \u2014 %ls", a->file_name);
    else wcscpy(title, L"HPCSV");
    title[511] = 0;
    SetWindowTextW(a->hwnd, title);
}

/* ------------------------------------------------------------------- undo */

static void undo_clear(App *a) {
    for (int i = 0; i < 64; i++) {
        for (size_t k = 0; k < a->undo[i].n; k++) {
            free(a->undo[i].cells[k].old_text);
            free(a->undo[i].cells[k].new_text);
        }
        free(a->undo[i].cells);
        a->undo[i].cells = NULL;
        a->undo[i].n = 0;
    }
    a->undo_pos = 0;
}

static void undo_push(App *a, UndoCell *cells, size_t n) {
    if (n == 0) { free(cells); return; }
    for (int i = a->undo_pos; i < 64; i++) {
        for (size_t k = 0; k < a->undo[i].n; k++) {
            free(a->undo[i].cells[k].old_text);
            free(a->undo[i].cells[k].new_text);
        }
        free(a->undo[i].cells);
        a->undo[i].cells = NULL;
        a->undo[i].n = 0;
    }
    if (a->undo_pos >= 64) {
        for (size_t k = 0; k < a->undo[0].n; k++) {
            free(a->undo[0].cells[k].old_text);
            free(a->undo[0].cells[k].new_text);
        }
        free(a->undo[0].cells);
        memmove(&a->undo[0], &a->undo[1], sizeof(UndoBatch) * 63);
        a->undo_pos = 63;
    }
    a->undo[a->undo_pos].cells = cells;
    a->undo[a->undo_pos].n = n;
    a->undo_pos++;
}

static void apply_cells_raw(App *a, HpCellEdit *edits, size_t n) {
    if (n == 0) return;
    hpcs_set_cells(&a->doc, edits, n);
    app_invalidate(a);
}

static void undo_redo(App *a, int redo) {
    if (redo) {
        if (a->undo_pos >= 64) return;
        UndoBatch *b = &a->undo[a->undo_pos];
        if (!b->cells || b->n == 0) return;
        HpCellEdit *edits = (HpCellEdit *)calloc(b->n, sizeof(HpCellEdit));
        if (!edits) return;
        for (size_t i = 0; i < b->n; i++) {
            edits[i].row = b->cells[i].row;
            edits[i].col = b->cells[i].col;
            edits[i].text = b->cells[i].new_text;
            edits[i].len = b->cells[i].new_len;
        }
        apply_cells_raw(a, edits, b->n);
        free(edits);
        a->undo_pos++;
        app_set_status(a, 0, L"Redo");
    } else {
        if (a->undo_pos <= 0) return;
        UndoBatch *b = &a->undo[a->undo_pos - 1];
        HpCellEdit *edits = (HpCellEdit *)calloc(b->n, sizeof(HpCellEdit));
        if (!edits) return;
        for (size_t i = 0; i < b->n; i++) {
            edits[i].row = b->cells[i].row;
            edits[i].col = b->cells[i].col;
            edits[i].text = b->cells[i].old_text;
            edits[i].len = b->cells[i].old_len;
        }
        apply_cells_raw(a, edits, b->n);
        free(edits);
        a->undo_pos--;
        app_set_status(a, 0, L"Undo");
    }
    a->dirty_save = 1;
    if (!a->save_timer) a->save_timer = SetTimer(a->hwnd, 1, 350, NULL);
    InvalidateRect(a->hwnd, NULL, FALSE);
}

void app_apply_edits(App *a, HpCellEdit *edits, size_t n) {
    if (n == 0 || !edits) return;
    UndoCell *uc = (UndoCell *)calloc(n, sizeof(UndoCell));
    if (!uc) return;
    size_t kept = 0;
    HpCellEdit *valid = (HpCellEdit *)calloc(n, sizeof(HpCellEdit));
    if (!valid) { free(uc); return; }
    for (size_t i = 0; i < n; i++) {
        uint32_t r = edits[i].row, c = edits[i].col;
        if (r >= a->doc.n_rows || c >= a->doc.rows[r].count) continue;
        size_t olen = 0;
        const char *otext = hpcs_cell_text(&a->doc, r, c, &olen);
        char *oc = (char *)malloc(olen + 1);
        char *nc = (char *)malloc(edits[i].len + 1);
        if (!oc || !nc) { free(oc); free(nc); continue; }
        memcpy(oc, otext, olen);
        oc[olen] = '\0';
        if (edits[i].len) memcpy(nc, edits[i].text, edits[i].len);
        nc[edits[i].len] = '\0';
        valid[kept] = edits[i];
        uc[kept].row = r;
        uc[kept].col = c;
        uc[kept].old_text = oc;
        uc[kept].old_len = olen;
        uc[kept].new_text = nc;
        uc[kept].new_len = edits[i].len;
        kept++;
    }
    if (kept == 0) { free(valid); free(uc); return; }

    HpStatus st = hpcs_set_cells(&a->doc, valid, kept);
    free(valid);
    if (st != HPCS_OK) {
        for (size_t i = 0; i < kept; i++) { free(uc[i].old_text); free(uc[i].new_text); }
        free(uc);
        app_set_status(a, 2, L"Edit failed (status %d)", (int)st);
        return;
    }
    undo_push(a, uc, kept);

    a->dirty_save = 1;
    if (!a->save_timer) a->save_timer = SetTimer(a->hwnd, 1, 350, NULL);
    app_set_status(a, 1, L"Modified \u2014 saving\u2026");
    InvalidateRect(a->hwnd, NULL, FALSE);
}

/* ------------------------------------------------------------------- save */

static void save_now(App *a) {
    if (a->save_timer) { KillTimer(a->hwnd, 1); a->save_timer = 0; }
    if (!a->path || !a->dirty_save) return;
    if (hpcs_save(&a->doc, a->path_u8) != HPCS_OK) {
        app_set_status(a, 3, L"Save failed \u2014 file locked?");
        return;
    }
    a->dirty_save = 0;
    a->external_change = 0;
    file_stamp(a, &a->last_write, &a->last_size);
    SYSTEMTIME st;
    GetLocalTime(&st);
    app_dbg("saved: bytes=%zu", a->doc.len);
    app_set_status(a, 0, L"Saved %02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
}

static void clamp_selection(App *a) {
    if (a->doc.n_rows == 0) {
        a->sel_row = a->anchor_row = 0;
        a->sel_col = a->anchor_col = 0;
        return;
    }
    if (a->sel_row >= (int)a->doc.n_rows) a->sel_row = (int)a->doc.n_rows - 1;
    if (a->sel_row < 0) a->sel_row = 0;
    if (a->anchor_row >= (int)a->doc.n_rows) a->anchor_row = (int)a->doc.n_rows - 1;
    if (a->anchor_row < 0) a->anchor_row = 0;
    uint32_t cols = a->doc.rows[a->sel_row].count;
    if (a->sel_col >= (int)cols) a->sel_col = (int)cols - 1;
    if (a->sel_col < 0) a->sel_col = 0;
    cols = a->doc.rows[a->anchor_row].count;
    if (a->anchor_col >= (int)cols) a->anchor_col = (int)cols - 1;
    if (a->anchor_col < 0) a->anchor_col = 0;
}

static void reload_external(App *a) {
    if (!a->path) return;
    HpDoc tmp;
    hpcs_init(&tmp);
    HpStatus st = hpcs_load(&tmp, a->path_u8);
    if (st != HPCS_OK) { hpcs_free(&tmp); return; }
    uint32_t old_cols = a->doc.max_cols;
    hpcs_free(&a->doc);
    a->doc = tmp;
    undo_clear(a);
    clamp_selection(a);
    app_metrics(a);
    if (a->doc.max_cols != old_cols || !a->colw) app_autofit_all(a);
    else app_ensure_cols(a);
    app_scrollbars(a);
    a->external_change = 0;
    file_stamp(a, &a->last_write, &a->last_size);
    app_set_status(a, 1, L"Reloaded from disk");
    app_dbg("reload: rows=%u cols=%u", a->doc.n_rows, a->doc.max_cols);
    InvalidateRect(a->hwnd, NULL, FALSE);
}

static void poll_disk(App *a) {
    if (!a->path) return;
    uint64_t wt = 0, sz = 0;
    file_stamp(a, &wt, &sz);
    if (wt == a->last_write && sz == a->last_size) return;
    if (a->editing) { a->external_change = 1; return; }
    if (a->dirty_save) {
        a->external_change = 1;
        app_set_status(a, 2, L"File changed on disk \u2014 local edits kept (Ctrl+R to reload)");
        return;
    }
    reload_external(a);
}

/* --------------------------------------------------------------- editing */

static void edit_commit(App *a, int dr, int dc);
static void edit_cancel(App *a);

static LRESULT CALLBACK edit_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    App *a = (App *)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (!a) return CallWindowProcW(g_edit_proc, h, msg, wp, lp);
    if (msg == WM_GETDLGCODE) return DLGC_WANTALLKEYS;
    if (msg == WM_KEYDOWN) {
        if (wp == VK_RETURN) {
            int dir = (GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1;
            edit_commit(a, dir, 0);
            return 0;
        }
        if (wp == VK_TAB) {
            int dir = (GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1;
            edit_commit(a, 0, dir);
            return 0;
        }
        if (wp == VK_ESCAPE) { edit_cancel(a); return 0; }
    }
    if (msg == WM_CHAR && (wp == L'\r' || wp == L'\n')) return 0;
    return CallWindowProcW(g_edit_proc, h, msg, wp, lp);
}

static void edit_begin(App *a, const wchar_t *initial) {
    if (a->editing || a->doc.n_rows == 0) return;
    clamp_selection(a);
    RECT rc;
    app_cell_rect(a, a->sel_row, a->sel_col, &rc);
    /* make sure the cell is on screen */
    HWND e = CreateWindowExW(0, L"EDIT", L"",
                             WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_LEFT,
                             rc.left + 1, rc.top + 2, rc.right - rc.left - 2,
                             rc.bottom - rc.top - 3, a->hwnd, (HMENU)1001,
                             GetModuleHandleW(NULL), NULL);
    if (!e) return;
    SendMessageW(e, WM_SETFONT, (WPARAM)a->font, TRUE);
    SetWindowLongPtrW(e, GWLP_USERDATA, (LONG_PTR)a);
    g_edit_proc = (WNDPROC)SetWindowLongPtrW(e, GWLP_WNDPROC, (LONG_PTR)edit_proc);

    size_t n = 0;
    const char *txt = hpcs_cell_text(&a->doc, a->sel_row, a->sel_col, &n);
    wchar_t *cur = utf8n_to_wide(txt, n);
    if (initial && initial[0]) SetWindowTextW(e, initial);
    else if (cur) SetWindowTextW(e, cur);
    free(cur);

    a->edit = e;
    a->editing = 1;
    a->edit_row = a->sel_row;
    a->edit_col = a->sel_col;
    SetFocus(e);
    SendMessageW(e, EM_SETSEL, 0, -1);
    app_set_status(a, 0, L"Editing \u2014 Enter to commit, Esc to cancel");
    InvalidateRect(a->hwnd, NULL, FALSE);
}

static void edit_commit(App *a, int dr, int dc) {
    if (!a->editing) return;
    HWND e = a->edit;
    int len = GetWindowTextLengthW(e);
    wchar_t *wbuf = (wchar_t *)malloc(((size_t)len + 1) * sizeof(wchar_t));
    if (wbuf) GetWindowTextW(e, wbuf, len + 1);
    a->editing = 0;
    a->edit = NULL;
    DestroyWindow(e);
    SetFocus(a->hwnd);

    if (wbuf) {
        char *u = wide_to_utf8(wbuf);
        if (u) {
            int r = a->edit_row, c = a->edit_col;
            size_t n = 0;
            const char *old = hpcs_cell_text(&a->doc, (uint32_t)r, (uint32_t)c, &n);
            int changed = !(strlen(u) == n && memcmp(u, old, n) == 0);
            if (changed) {
                app_dbg("commit: r=%d c=%d textlen=%zu oldlen=%zu doc=%zu",
                        r, c, strlen(u), n, a->doc.len);
                HpCellEdit ed;
                ed.row = (uint32_t)r;
                ed.col = (uint32_t)c;
                ed.text = u;
                ed.len = strlen(u);
                app_apply_edits(a, &ed, 1);
            }
            free(u);
        }
        free(wbuf);
    }

    if (a->doc.n_rows > 0) {
        int r = a->edit_row + dr;
        int c = a->edit_col + dc;
        if (r < 0) r = 0;
        if (r >= (int)a->doc.n_rows) r = (int)a->doc.n_rows - 1;
        uint32_t cols = a->doc.rows[r].count;
        if (c < 0) c = 0;
        if (dc > 0 && c >= (int)cols) {
            if (r + 1 < (int)a->doc.n_rows) { r++; c = 0; }
            else c = (int)cols - 1;
        }
        if (c >= (int)cols) c = (int)cols - 1;
        a->sel_row = a->anchor_row = r;
        a->sel_col = a->anchor_col = c;
    }
    app_invalidate(a);
}

static void edit_cancel(App *a) {
    if (!a->editing) return;
    HWND e = a->edit;
    a->editing = 0;
    a->edit = NULL;
    DestroyWindow(e);
    SetFocus(a->hwnd);
    app_set_status(a, 0, L"Edit cancelled");
    app_invalidate(a);
}

/* -------------------------------------------------------------- selection */

static void ensure_visible(App *a, int row, int col) {
    if (row <= 0 || a->doc.n_rows == 0) return;
    int vw = app_view_w(a), vh = app_view_h(a);
    int top = (row - 1) * a->row_h;
    if (top < a->scroll_y) a->scroll_y = top;
    if (top + a->row_h > a->scroll_y + vh) a->scroll_y = top + a->row_h - vh;
    if (a->scroll_y < 0) a->scroll_y = 0;

    int x = app_colx(a, (uint32_t)col);
    int w = (col < (int)a->ncols) ? a->colw[col] : 120;
    if (x < a->scroll_x) a->scroll_x = x;
    if (x + w > a->scroll_x + vw) a->scroll_x = x + w - vw;
    if (a->scroll_x < 0) a->scroll_x = 0;
    app_scrollbars(a);
}

static void append_row(App *a) {
    if (a->editing) edit_commit(a, 0, 0);
    uint32_t cols = a->doc.max_cols ? a->doc.max_cols : 1;
    if (hpcs_append_row(&a->doc, cols) != HPCS_OK) return;
    a->sel_row = a->anchor_row = (int)a->doc.n_rows - 1;
    a->sel_col = a->anchor_col = 0;
    if (a->doc.rows[a->sel_row].count == 0) {
        a->sel_row = a->anchor_row = 0;
    }
    a->dirty_save = 1;
    if (!a->save_timer) a->save_timer = SetTimer(a->hwnd, 1, 350, NULL);
    app_ensure_cols(a);
    app_scrollbars(a);
    ensure_visible(a, a->sel_row, 0);
    app_set_status(a, 1, L"Row added");
    app_invalidate(a);
}

static void move_selection(App *a, int dr, int dc, int extend) {
    if (a->doc.n_rows == 0) return;
    clamp_selection(a);
    int r = a->sel_row + dr;
    int c = a->sel_col + dc;
    if (r < 0) r = 0;
    if (r >= (int)a->doc.n_rows) r = (int)a->doc.n_rows - 1;
    uint32_t cols = a->doc.rows[r].count;
    if (c < 0) c = 0;
    if (c >= (int)cols) c = (int)cols - 1;
    a->sel_row = r;
    a->sel_col = c;
    if (!extend) { a->anchor_row = r; a->anchor_col = c; }
    ensure_visible(a, r, c);
    app_invalidate(a);
}

static void selection_bounds(App *a, int *r0, int *r1, int *c0, int *c1) {
    int ar = a->anchor_row, as = a->anchor_col, sr = a->sel_row, sc = a->sel_col;
    *r0 = ar < sr ? ar : sr;
    *r1 = ar > sr ? ar : sr;
    *c0 = as < sc ? as : sc;
    *c1 = as > sc ? as : sc;
}

/* ------------------------------------------------------------- clipboard */

static void copy_selection(App *a) {
    int r0, r1, c0, c1;
    selection_bounds(a, &r0, &r1, &c0, &c1);
    if (r0 > r1 || c0 > c1) return;
    size_t cap = 4096, len = 0;
    char *out = (char *)malloc(cap);
    if (!out) return;
    out[0] = '\0';
    for (int r = r0; r <= r1; r++) {
        for (int c = c0; c <= c1; c++) {
            size_t n = 0;
            const char *t = hpcs_cell_text(&a->doc, (uint32_t)r, (uint32_t)c, &n);
            int need_q = 0;
            for (size_t k = 0; k < n; k++)
                if (t[k] == '\t' || t[k] == '\n' || t[k] == '\r' || t[k] == '"') need_q = 1;
            size_t need = n * 2 + 4;
            if (len + need > cap) {
                while (len + need > cap) cap *= 2;
                char *p = (char *)realloc(out, cap);
                if (!p) { free(out); return; }
                out = p;
            }
            if (c > c0) out[len++] = '\t';
            if (need_q) {
                out[len++] = '"';
                for (size_t k = 0; k < n; k++) {
                    if (t[k] == '"') out[len++] = '"';
                    out[len++] = t[k];
                }
                out[len++] = '"';
            } else {
                memcpy(out + len, t, n);
                len += n;
            }
        }
        out[len++] = '\r';
        out[len++] = '\n';
    }
    out[len] = '\0';

    wchar_t *w = utf8_to_wide(out);
    free(out);
    if (!w) return;
    if (OpenClipboard(a->hwnd)) {
        EmptyClipboard();
        size_t bytes = (wcslen(w) + 1) * sizeof(wchar_t);
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (h) {
            void *p = GlobalLock(h);
            if (p) { memcpy(p, w, bytes); GlobalUnlock(h); SetClipboardData(CF_UNICODETEXT, h); }
        }
        CloseClipboard();
    }
    free(w);
    app_set_status(a, 0, L"Copied %d \u00d7 %d cells", c1 - c0 + 1, r1 - r0 + 1);
}

static void open_dialog(App *a) {
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    wchar_t file[MAX_PATH] = L"";
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = a->hwnd;
    ofn.lpstrFilter = L"CSV / TSV\0*.csv;*.tsv;*.txt\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) app_open_file(a, file);
}

static void open_dialog(App *a);

static void paste_clipboard(App *a) {
    if (a->doc.n_rows == 0) return;
    if (!OpenClipboard(a->hwnd)) return;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (!h) { CloseClipboard(); return; }
    wchar_t *w = (wchar_t *)GlobalLock(h);
    char *u = w ? wide_to_utf8(w) : NULL;
    if (w) GlobalUnlock(h);
    CloseClipboard();
    if (!u) return;

    size_t n = strlen(u);
    size_t cap = 64, count = 0;
    HpCellEdit *edits = (HpCellEdit *)calloc(cap, sizeof(HpCellEdit));
    char **owned = (char **)calloc(cap, sizeof(char *));
    if (!edits || !owned) { free(edits); free(owned); free(u); return; }

    size_t i = 0;
    int row = a->sel_row;
    while (i <= n && row < (int)a->doc.n_rows) {
        int col = a->sel_col;
        int line_done = 0;
        while (!line_done) {
            size_t bcap = 64, blen = 0;
            char *buf = (char *)malloc(bcap);
            if (!buf) break;
            if (i < n && u[i] == '"') {
                i++;
                while (i < n) {
                    if (u[i] == '"') {
                        if (i + 1 < n && u[i + 1] == '"') i++;
                        else { i++; break; }
                    }
                    if (blen + 2 > bcap) { bcap *= 2; buf = (char *)realloc(buf, bcap); }
                    buf[blen++] = u[i++];
                }
            } else {
                while (i < n && u[i] != '\t' && u[i] != '\n' && u[i] != '\r') {
                    if (blen + 2 > bcap) { bcap *= 2; buf = (char *)realloc(buf, bcap); }
                    buf[blen++] = u[i++];
                }
            }
            buf[blen] = '\0';

            if (col < (int)a->doc.rows[row].count) {
                if (count == cap) {
                    cap *= 2;
                    edits = (HpCellEdit *)realloc(edits, cap * sizeof(HpCellEdit));
                    owned = (char **)realloc(owned, cap * sizeof(char *));
                }
                edits[count].row = (uint32_t)row;
                edits[count].col = (uint32_t)col;
                edits[count].text = buf;
                edits[count].len = blen;
                owned[count] = buf;
                count++;
            } else {
                free(buf);
            }

            if (i < n && u[i] == '\t') { i++; col++; continue; }
            if (i < n && u[i] == '\r') { i++; if (i < n && u[i] == '\n') i++; }
            else if (i < n && u[i] == '\n') i++;
            line_done = 1;
        }
        row++;
        if (i >= n) break;
    }

    if (count) app_apply_edits(a, edits, count);
    else app_set_status(a, 2, L"Nothing pasted \u2014 selection may be at the sheet edge");
    for (size_t k = 0; k < count; k++) free(owned[k]);
    free(owned);
    free(edits);
    free(u);
}

static void clear_selection(App *a) {
    int r0, r1, c0, c1;
    selection_bounds(a, &r0, &r1, &c0, &c1);
    size_t count = 0, cap = 64;
    HpCellEdit *edits = (HpCellEdit *)calloc(cap, sizeof(HpCellEdit));
    for (int r = r0; r <= r1; r++) {
        for (int c = c0; c <= c1 && c < (int)a->doc.rows[r].count; c++) {
            if (count == cap) { cap *= 2; edits = (HpCellEdit *)realloc(edits, cap * sizeof(HpCellEdit)); }
            edits[count].row = (uint32_t)r;
            edits[count].col = (uint32_t)c;
            edits[count].text = "";
            edits[count].len = 0;
            count++;
        }
    }
    if (count) app_apply_edits(a, edits, count);
    free(edits);
}

/* ----------------------------------------------------------- open / close */

void app_close_file(App *a) {
    hpcs_free(&a->doc);
    hpcs_init(&a->doc);
    free(a->path); a->path = NULL;
    free(a->path_u8); a->path_u8 = NULL;
    a->file_name[0] = 0;
    a->ncols = 0;
    undo_clear(a);
    a->dirty_save = 0;
    a->external_change = 0;
    update_title(a);
}

int app_open_file(App *a, const wchar_t *path) {
    if (a->editing) edit_commit(a, 0, 0);
    if (a->dirty_save) save_now(a);
    char *u = wide_to_utf8(path);
    if (!u) return 0;

    HpDoc tmp;
    hpcs_init(&tmp);
    HpStatus st = hpcs_load(&tmp, u);
    if (st != HPCS_OK) {
        wchar_t msg[512];
        _snwprintf(msg, 511, L"Cannot open:\n%ls", path);
        MessageBoxW(a->hwnd, msg, L"HPCSV", MB_ICONERROR | MB_OK);
        free(u);
        hpcs_free(&tmp);
        return 0;
    }

    if (a->save_timer) { KillTimer(a->hwnd, 1); a->save_timer = 0; }
    app_close_file(a);
    a->doc = tmp;
    a->path = _wcsdup(path);
    a->path_u8 = u;
    const wchar_t *base = path;
    for (const wchar_t *p = path; *p; p++)
        if (*p == L'\\' || *p == L'/') base = p + 1;
    _snwprintf(a->file_name, 259, L"%ls", base);
    a->file_name[259] = 0;

    file_stamp(a, &a->last_write, &a->last_size);
    a->scroll_x = a->scroll_y = 0;
    a->sel_row = a->anchor_row = (a->doc.n_rows > 1) ? 1 : 0;
    a->sel_col = a->anchor_col = 0;
    if (a->sel_row >= (int)a->doc.n_rows) a->sel_row = (int)a->doc.n_rows - 1;
    if (a->sel_row < 0) a->sel_row = 0;

    app_metrics(a);
    app_autofit_all(a);
    app_scrollbars(a);
    update_title(a);
    a->dirty_save = 0;
    a->external_change = 0;

    char dbuf[32];
    hpcs_delimiter_name(a->doc.delim, dbuf, sizeof(dbuf));
    wchar_t extra[64] = L"";
    if (a->doc.ragged_rows)
        _snwprintf(extra, 63, L" \u00b7 %u irregular row(s)", a->doc.ragged_rows);
    extra[63] = 0;
    app_set_status(a, 0, L"%u rows \u00d7 %u cols \u00b7 %hs%ls",
                   a->doc.n_rows, a->doc.max_cols, dbuf, extra);
    InvalidateRect(a->hwnd, NULL, FALSE);
    dbg_log("open: rows=%u cols=%u sel=(%d,%d) anchor=(%d,%d) dpi=%d",
            a->doc.n_rows, a->doc.max_cols, a->sel_row, a->sel_col,
            a->anchor_row, a->anchor_col, a->dpi);
    return 1;
}

/* ------------------------------------------------------------- mouse/key */

static int hit_row(App *a, int y) {
    if (y < a->header_h) return 0;
    int by = y - a->header_h + a->scroll_y;
    if (by < 0) return -1;
    int r = 1 + by / a->row_h;
    if (r >= (int)a->doc.n_rows) return -1;
    return r;
}

static int hit_col(App *a, int x) {
    if (a->ncols == 0) return -1;
    if (x < a->gutter_w) return -1;
    int c = app_col_at(a, x);
    if (c >= (int)a->ncols) c = (int)a->ncols - 1;
    return c;
}

static void on_lbutton_down(App *a, int x, int y) {
    SetFocus(a->hwnd);
    if (a->editing) edit_commit(a, 0, 0);

    if (y < a->header_h) {
        /* column resize handle */
        if (a->ncols > 0) {
            int c = app_col_at(a, x);
            if (c >= 0 && c < (int)a->ncols) {
                int right = a->gutter_w + app_colx(a, (uint32_t)c + 1) - a->scroll_x;
                if (abs(x - right) <= 4) {
                    a->resizing_col = c;
                    a->resize_x = x;
                    a->resize_w = a->colw[c];
                    SetCapture(a->hwnd);
                    SetCursor(LoadCursorW(NULL, IDC_SIZEWE));
                    return;
                }
            }
        }
        int c = hit_col(a, x);
        if (c < 0) return;
        a->sel_row = a->anchor_row = 0;
        a->sel_col = a->anchor_col = c;
    } else if (x < a->gutter_w) {
        int r = hit_row(a, y);
        if (r < 0) return;
        a->sel_row = a->anchor_row = r;
        a->sel_col = 0;
        a->anchor_col = (int)a->doc.rows[r].count - 1;
    } else {
        int r = hit_row(a, y);
        int c = hit_col(a, x);
        if (r < 0 || c < 0) return;
        a->sel_row = a->anchor_row = r;
        a->sel_col = a->anchor_col = c;
        a->dragging = 1;
        SetCapture(a->hwnd);
    }
    app_invalidate(a);
}

static void on_mouse_move(App *a, int x, int y) {
    if (a->resizing_col >= 0) {
        int w = a->resize_w + (x - a->resize_x);
        int minw = 40;
        if (w < minw) w = minw;
        if (w > 2000) w = 2000;
        a->colw[a->resizing_col] = w;
        app_ensure_cols(a);
        app_scrollbars(a);
        app_invalidate(a);
        return;
    }
    if (!a->dragging) {
        if (y < a->header_h && a->ncols > 0) {
            int c = app_col_at(a, x);
            if (c >= 0 && c < (int)a->ncols) {
                int right = a->gutter_w + app_colx(a, (uint32_t)c + 1) - a->scroll_x;
                if (abs(x - right) <= 4) { SetCursor(LoadCursorW(NULL, IDC_SIZEWE)); return; }
            }
        }
        SetCursor(LoadCursorW(NULL, IDC_ARROW));
        return;
    }
    int r = hit_row(a, y);
    int c = hit_col(a, x);
    if (r < 0) r = (y < a->header_h) ? 0 : a->sel_row;
    if (c < 0) c = a->sel_col;
    a->sel_row = r;
    a->sel_col = c;
    uint32_t cols = a->doc.rows[r].count;
    if (a->sel_col >= (int)cols) a->sel_col = (int)cols - 1;
    app_invalidate(a);
}

static void on_lbutton_up(App *a) {
    if (a->resizing_col >= 0) {
        a->resizing_col = -1;
        ReleaseCapture();
    }
    if (a->dragging) { a->dragging = 0; ReleaseCapture(); }
}

static void open_dialog(App *a);

static void on_key(App *a, WPARAM key) {
    int shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    int ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    if (ctrl && key == 'O') { open_dialog(a); return; }
    if (a->doc.n_rows == 0) {
        if (key == VK_RETURN || key == VK_F2) {
            append_row(a);
            edit_begin(a, NULL);
        }
        return;
    }
    switch (key) {
    case VK_LEFT:
        if (ctrl) move_selection(a, 0, -(a->sel_col + 1), shift);
        else move_selection(a, 0, -1, shift);
        return;
    case VK_RIGHT:
        if (ctrl) move_selection(a, 0, (int)a->doc.rows[a->sel_row].count, shift);
        else move_selection(a, 0, 1, shift);
        return;
    case VK_UP:
        if (ctrl) move_selection(a, -(a->sel_row + 1), 0, shift);
        else move_selection(a, -1, 0, shift);
        return;
    case VK_DOWN:
        if (ctrl) move_selection(a, (int)a->doc.n_rows, 0, shift);
        else move_selection(a, 1, 0, shift);
        return;
    case VK_PRIOR: move_selection(a, -(app_view_h(a) / a->row_h), 0, shift); return;
    case VK_NEXT: move_selection(a, app_view_h(a) / a->row_h, 0, shift); return;
    case VK_HOME:
        if (ctrl) move_selection(a, -(a->sel_row + 1), -(a->sel_col + 1), shift);
        else move_selection(a, 0, -(a->sel_col + 1), shift);
        return;
    case VK_END:
        if (ctrl) move_selection(a, (int)a->doc.n_rows, (int)a->doc.max_cols, shift);
        else move_selection(a, 0, (int)a->doc.rows[a->sel_row].count, shift);
        return;
    case VK_TAB:
        move_selection(a, 0, shift ? -1 : 1, 0);
        return;
    case VK_RETURN:
        if (ctrl) append_row(a);
        else edit_begin(a, NULL);
        return;
    case VK_F2:
        if (a->doc.n_rows == 0) return;
        edit_begin(a, NULL);
        return;
    case VK_DELETE:
        if (a->doc.n_rows && !a->editing) clear_selection(a);
        return;
    case VK_ESCAPE:
        a->sel_row = a->anchor_row;
        a->sel_col = a->anchor_col;
        app_invalidate(a);
        return;
    default:
        break;
    }

    if (ctrl) {
        switch (key) {
        case 'A':
            if (a->doc.n_rows) {
                a->anchor_row = 0; a->anchor_col = 0;
                a->sel_row = (int)a->doc.n_rows - 1;
                a->sel_col = (int)a->doc.rows[a->sel_row].count - 1;
                app_invalidate(a);
            }
            return;
        case 'C': copy_selection(a); return;
        case 'V': paste_clipboard(a); return;
        case 'S': save_now(a); return;
        case 'R':
            if (a->dirty_save) save_now(a);
            reload_external(a);
            return;
        case 'Z': undo_redo(a, 0); return;
        case 'Y': undo_redo(a, 1); return;
        default: break;
        }
    }
}

static void on_char(App *a, WPARAM ch) {
    if (a->editing) return;
    if (ch < 32) return;
    if (a->doc.n_rows == 0) {
        uint32_t cols = a->doc.max_cols ? a->doc.max_cols : 1;
        if (hpcs_append_row(&a->doc, cols) != HPCS_OK) return;
        a->sel_row = a->anchor_row = (int)a->doc.n_rows - 1;
        a->sel_col = a->anchor_col = 0;
        a->dirty_save = 1;
        if (!a->save_timer) a->save_timer = SetTimer(a->hwnd, 1, 350, NULL);
        app_ensure_cols(a);
        app_scrollbars(a);
    }
    wchar_t w[2] = { (wchar_t)ch, 0 };
    edit_begin(a, w);
}

/* --------------------------------------------------------------- window */

static void update_scrollbars_now(App *a) {
    app_scrollbars(a);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    App *a = &g_app;
    switch (msg) {
    case WM_CREATE:
        a->hwnd = hwnd;
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        app_metrics(a);
        app_scrollbars(a);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        {
            static int logged = 0;
            if (!logged) {
                logged = 1;
                dbg_log("paint: w=%d h=%d sel=(%d,%d) anchor=(%d,%d) header=%d row=%d gutter=%d",
                        rc.right, rc.bottom, a->sel_row, a->sel_col,
                        a->anchor_row, a->anchor_col, a->header_h, a->row_h, a->gutter_w);
            }
        }
        app_paint(a, hdc, rc.right, rc.bottom);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SETFOCUS:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_KILLFOCUS:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_TIMER:
        if (wp == 1) { save_now(a); return 0; }
        if (wp == 2) { poll_disk(a); return 0; }
        break;
    case WM_VSCROLL: {
        SCROLLINFO si;
        si.cbSize = sizeof(si);
        si.fMask = SIF_ALL;
        GetScrollInfo(hwnd, SB_VERT, &si);
        int pos = si.nPos;
        switch (LOWORD(wp)) {
        case SB_LINEUP: pos -= a->row_h; break;
        case SB_LINEDOWN: pos += a->row_h; break;
        case SB_PAGEUP: pos -= app_view_h(a); break;
        case SB_PAGEDOWN: pos += app_view_h(a); break;
        case SB_THUMBTRACK: pos = si.nTrackPos; break;
        case SB_TOP: pos = si.nMin; break;
        case SB_BOTTOM: pos = si.nMax; break;
        default: break;
        }
        int vh = app_view_h(a);
        int total = a->doc.n_rows > 1 ? (int)(a->doc.n_rows - 1) * a->row_h : 0;
        int maxp = total - vh;
        if (maxp < 0) maxp = 0;
        if (pos < 0) pos = 0;
        if (pos > maxp) pos = maxp;
        a->scroll_y = pos;
        app_scrollbars(a);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_HSCROLL: {
        SCROLLINFO si;
        si.cbSize = sizeof(si);
        si.fMask = SIF_ALL;
        GetScrollInfo(hwnd, SB_HORZ, &si);
        int pos = si.nPos;
        switch (LOWORD(wp)) {
        case SB_LINELEFT: pos -= 48; break;
        case SB_LINERIGHT: pos += 48; break;
        case SB_PAGELEFT: pos -= app_view_w(a); break;
        case SB_PAGERIGHT: pos += app_view_w(a); break;
        case SB_THUMBTRACK: pos = si.nTrackPos; break;
        default: break;
        }
        int vw = app_view_w(a);
        int total = (a->ncols && a->colx) ? a->colx[a->ncols] : 0;
        int maxp = total - vw;
        if (maxp < 0) maxp = 0;
        if (pos < 0) pos = 0;
        if (pos > maxp) pos = maxp;
        a->scroll_x = pos;
        app_scrollbars(a);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        int shift = (GET_KEYSTATE_WPARAM(wp) & MK_SHIFT) != 0;
        if (shift) {
            a->scroll_x -= delta * 2;
            int maxx = (int)(a->ncols ? a->colx[a->ncols] : 0) - app_view_w(a);
            if (a->scroll_x < 0) a->scroll_x = 0;
            if (maxx < 0) maxx = 0;
            if (a->scroll_x > maxx) a->scroll_x = maxx;
        } else {
            a->scroll_y -= (delta / WHEEL_DELTA) * a->row_h * 3;
            int total = (int)(a->doc.n_rows > 0 ? (a->doc.n_rows - 1) * a->row_h : 0);
            int maxy = total - app_view_h(a);
            if (a->scroll_y < 0) a->scroll_y = 0;
            if (maxy < 0) maxy = 0;
            if (a->scroll_y > maxy) a->scroll_y = maxy;
        }
        app_scrollbars(a);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_LBUTTONDOWN:
        on_lbutton_down(a, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_LBUTTONDBLCLK: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (y < a->header_h && a->ncols > 0) {
            int c = app_col_at(a, x);
            if (c >= 0 && c < (int)a->ncols) {
                int right = a->gutter_w + app_colx(a, (uint32_t)c + 1) - a->scroll_x;
                if (abs(x - right) <= 4) {
                    app_autofit_one(a, (uint32_t)c);
                    app_scrollbars(a);
                    InvalidateRect(hwnd, NULL, FALSE);
                    return 0;
                }
            }
        }
        int r = hit_row(a, y);
        int c = hit_col(a, x);
        if (r >= 0 && c >= 0) {
            a->sel_row = a->anchor_row = r;
            a->sel_col = a->anchor_col = c;
            edit_begin(a, NULL);
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        on_mouse_move(a, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_LBUTTONUP:
        on_lbutton_up(a);
        return 0;
    case WM_KEYDOWN:
        on_key(a, wp);
        return 0;
    case WM_CHAR:
        on_char(a, wp);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == 1001 && HIWORD(wp) == EN_KILLFOCUS) {
            if (a->editing) edit_commit(a, 0, 0);
        }
        return 0;
    case WM_DROPFILES: {
        HDROP drop = (HDROP)wp;
        wchar_t path[MAX_PATH];
        if (DragQueryFileW(drop, 0, path, MAX_PATH)) app_open_file(a, path);
        DragFinish(drop);
        return 0;
    }
    case WM_CLOSE:
        if (a->editing) edit_commit(a, 0, 0);
        if (a->dirty_save) save_now(a);
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (a->save_timer) KillTimer(hwnd, 1);
        if (a->watch_timer) KillTimer(hwnd, 2);
        if (a->mem_dc) { DeleteDC(a->mem_dc); a->mem_dc = NULL; }
        if (a->mem_bmp) { DeleteObject(a->mem_bmp); a->mem_bmp = NULL; }
        if (a->font) DeleteObject(a->font);
        if (a->font_bold) DeleteObject(a->font_bold);
        if (a->font_small) DeleteObject(a->font_small);
        free(a->colw);
        free(a->colx);
        undo_clear(a);
        hpcs_free(&a->doc);
        free(a->path);
        free(a->path_u8);
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void create_fonts(App *a) {
    int h = -MulDiv(10, a->dpi, 72);
    a->font = CreateFontW(h, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                          OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    a->font_bold = CreateFontW(h, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    int hs = -MulDiv(8, a->dpi, 72);
    a->font_small = CreateFontW(hs, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HDC dc = GetDC(NULL);
    HGDIOBJ old = SelectObject(dc, a->font);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    a->font_h = tm.tmHeight;
    SelectObject(dc, old);
    ReleaseDC(NULL, dc);
}

static int get_dpi(void) {
    HDC dc = GetDC(NULL);
    int dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(NULL, dc);
    if (dpi <= 0) dpi = 96;
    return dpi;
}

static void save_bmp(HDC dc, int w, int h, const char *path) {
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!dib || !bits) { if (dib) DeleteObject(dib); DeleteDC(mem); return; }
    HGDIOBJ old = SelectObject(mem, dib);
    BitBlt(mem, 0, 0, w, h, dc, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteDC(mem);

    BITMAPFILEHEADER fh;
    memset(&fh, 0, sizeof(fh));
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fh.bfSize = fh.bfOffBits + (DWORD)w * (DWORD)h * 4;
    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(&fh, sizeof(fh), 1, f);
        fwrite(&bi.bmiHeader, sizeof(BITMAPINFOHEADER), 1, f);
        fwrite(bits, 1, (size_t)w * (size_t)h * 4, f);
        fclose(f);
    }
    DeleteObject(dib);
}

int main(void) {
    SetProcessDPIAware();
#ifdef _WIN32
    if (!getenv("HPCS_DEBUG")) {
        HWND con = GetConsoleWindow();
        if (con) ShowWindow(con, SW_HIDE);
        FreeConsole();
    }
#endif
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    int snapshot = 0;
    const wchar_t *filearg = NULL;
    if (argc >= 2) {
        if (wcscmp(argv[1], L"--snapshot") == 0 && argc >= 4) {
            snapshot = 1;
            filearg = argv[2];
        } else {
            filearg = argv[1];
        }
    }

    App *a = &g_app;
    memset(a, 0, sizeof(*a));
    hpcs_init(&a->doc);
    a->resizing_col = -1;
    a->dpi = get_dpi();
    a->status_kind = 0;
    a->hint[0] = 0;

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wc.hIconSm = LoadIconW(NULL, IDI_APPLICATION);
    wc.hbrBackground = NULL;
    wc.lpszClassName = HPCS_CLASS;
    if (!RegisterClassExW(&wc)) return 1;

    int w = MulDiv(1240, a->dpi, 96), hgt = MulDiv(780, a->dpi, 96);
    RECT wr = { 0, 0, w, hgt };
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW | WS_VSCROLL | WS_HSCROLL, FALSE);
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    HWND hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, HPCS_CLASS, HPCS_TITLE,
                                WS_OVERLAPPEDWINDOW | WS_VSCROLL | WS_HSCROLL,
                                (sw - (wr.right - wr.left)) / 2,
                                (sh - (wr.bottom - wr.top)) / 2,
                                wr.right - wr.left, wr.bottom - wr.top,
                                NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) return 1;
    a->hwnd = hwnd;
    DragAcceptFiles(hwnd, TRUE);

    create_fonts(a);
    app_metrics(a);
    app_ensure_cols(a);

    if (filearg) app_open_file(a, filearg);
    else open_dialog(a);

    if (snapshot) {
        int sw2 = 1400, sh2 = 900;
        HDC screen = GetDC(NULL);
        HDC dc = CreateCompatibleDC(screen);
        HBITMAP bmp = CreateCompatibleBitmap(screen, sw2, sh2);
        HGDIOBJ old = SelectObject(dc, bmp);
        app_paint(a, dc, sw2, sh2);
        char out[MAX_PATH];
        WideCharToMultiByte(CP_UTF8, 0, argv[3], -1, out, MAX_PATH, NULL, NULL);
        save_bmp(dc, sw2, sh2, out);
        SelectObject(dc, old);
        DeleteObject(bmp);
        DeleteDC(dc);
        ReleaseDC(NULL, screen);
        DestroyWindow(hwnd);
        LocalFree(argv);
        return 0;
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    a->watch_timer = SetTimer(hwnd, 2, 350, NULL);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    LocalFree(argv);
    return 0;
}
