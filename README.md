<div align="center">

<img src="docs/screenshot.png" alt="HPCSV — a native spreadsheet for CSV files" width="760">

# HPCSV

**Open any `.csv` file as a real spreadsheet.**
A dependency-free C core, plus a Zed extension that keeps the editor tab as the source of truth.

<p>
  <a href="LICENSE"><img src="https://img.shields.io/github/license/RedTheFoxx/HPCSV?style=flat-square&color=3b82f6" alt="MIT License"></a>
  <img src="https://img.shields.io/badge/Zig-0.17.0-f7a41d?style=flat-square&logo=zig&logoColor=white" alt="Zig 0.17.0">
  <img src="https://img.shields.io/badge/C-23-00599C?style=flat-square&logo=c&logoColor=white" alt="C23">
  <img src="https://img.shields.io/badge/Windows-10%2F11-0078D6?style=flat-square&logo=windows11&logoColor=white" alt="Windows 10/11">
  <img src="https://img.shields.io/badge/Zed-extension-084CCF?style=flat-square&logo=zedindustries&logoColor=white" alt="Zed extension">
  <a href="https://github.com/RedTheFoxx/HPCSV/stargazers"><img src="https://img.shields.io/github/stars/RedTheFoxx/HPCSV?style=flat-square&color=f5c518" alt="Stars"></a>
</p>

</div>

Three binaries, one core:

| Binary | Role |
| --- | --- |
| **`HPCSV.exe`** | Native Win32/GDI spreadsheet: virtualized grid, editing, undo/redo, clipboard. |
| **`hpcs-lsp.exe`** | C language server: diagnostics, cell hover, an "open in HPCSV" code action. |
| **`hpcs.exe`** | CLI and diagnostic tool (`info`, `get`, `set`, `dump`, `bench`). |

> **Why a native binary?** Zed extensions cannot draw custom views (webview/panel) — the
> *Visual Extension API* RFC is still open. HPCSV takes the opposite approach: **the Zed
> CSV tab stays the source of truth**, the native spreadsheet edits **the same file**, and
> the two stay in sync live. A real spreadsheet, without forking Zed.

---

## Features

**Spreadsheet** — virtualized grid (smooth on millions of rows) · in-place editing · frozen
header and resizable columns · rectangular selection · TSV copy/paste compatible with
Excel/LibreOffice · multi-cell undo/redo · separator detection (`, ; \t |`) · Unicode paths
and drag & drop · atomic saves (BOM, CRLF and quoting preserved) · automatic reload when the
file changes on disk.

**Zed extension** — **CSV** and **TSV** languages (tree-sitter, highlighting) · diagnostics
(field count ≠ header, unterminated quote) · hover (address, column, type, row summary) ·
**`HPCSV: open in spreadsheet`** code action (`Ctrl+.`).

**C core** — tolerant RFC 4180 parser (quoted fields, `""`, embedded newlines, BOM, final
line without `\n`) · zero-copy index (no per-cell `malloc`) · splice-based editing with no
full reparse.

---

## Quick start

```powershell
git clone https://github.com/RedTheFoxx/HPCSV
cd HPCSV
zig build -Doptimize=ReleaseFast
.\zig-out\bin\HPCSV.exe samples\demo.csv
```

**Zed extension** — after `zig build`, open `zed: extensions` → **Install Dev Extension** →
the `extension/` folder. The `hpcs-lsp` server is resolved automatically: the repository's
`zig-out\bin`, `HPCSV_LSP` (full path), the `HPCSV_BIN` directory, then `PATH`.

---

## Usage

```powershell
hpcs info  samples\demo.csv
hpcs get   samples\demo.csv 2 3          # row 2, column 3 (1-based)
hpcs set   samples\demo.csv 2 3 "Paris"
hpcs dump  samples\demo.csv 10           # TSV of the first 10 rows
hpcs bench big.csv 10                    # parsing throughput
```

<details>
<summary><b>Spreadsheet shortcuts</b></summary>

| Shortcut | Action |
| --- | --- |
| `↑ ↓ ← →` / `Shift + arrows` | move / extend the selection |
| `Ctrl + arrows`, `Home`/`End` | edges of the data, the row, the document |
| `Enter` / `F2` / direct typing | edit the cell |
| `Enter` / `Tab` / `Shift + Tab` | commit and move down / right / left |
| `Escape` | cancel the edit or the selection |
| `Delete` | clear the selection |
| `Ctrl + C` / `Ctrl + V` | copy / paste as TSV |
| `Ctrl + Z` / `Ctrl + Y` | undo / redo |
| `Ctrl + S` / `Ctrl + R` | save / reload |
| `Ctrl + A` / `Ctrl + O` | select all / open a file |
| `Ctrl + Enter` | add a row |

Click-drag: rectangular selection. Double-click: edit. Header border: resize. Wheel:
vertical; `Shift + wheel`: horizontal.

</details>

---

## Performance

`ReleaseFast` build, 80.26 MiB file (1,000,001 rows × 8 columns) — measured on Zig 0.14.1:

| Operation | Time |
| --- | --- |
| Full parse | **79 ms** (≈ 1.0 GiB/s) |
| Edit one cell + atomic rewrite | **~190 ms** |
| App: parse + open | ~0.2 s |

Parsing is single-pass and zero-copy; editing does not reparse the document: it replaces the
cell's raw byte range and shifts the following offsets.

---

## Architecture

```
   Zed tab ──▶┌──────────────────────────┐◀── atomic save
   (editing)  │  .csv file on disk       │
              └────────────┬─────────────┘
                           │ mtime watch
              ┌────────────▼─────────────┐        ┌──────────────────┐
              │  HPCSV.exe               │        │  hpcs-lsp.exe    │
              │  C core + Win32          │        │  C, JSON-RPC     │
              └──────────────────────────┘        └──────────────────┘
                                                     ▲  diagnostics / hover
```

```
HPCSV/
├── core/        hpcs.h + hpcs.c      parser, index, editing, I/O
├── core/tests/  C test suite
├── app/         Win32 spreadsheet (main.c, render.c)
├── lsp/         C language server
├── cli/         hpcs CLI
├── extension/   Zed extension (Rust → wasm32-wasip2)
└── samples/     demo data sets
```

Design details: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

---

## Building

Requirements: **Zig 0.17.0** (the `build.zig` targets its API) · **Windows 10/11** for the
native spreadsheet (core and LSP are portable C23) · **Rust** (`wasm32-wasip2`) for the
extension.

```powershell
zig build                          # debug
zig build -Doptimize=ReleaseFast   # release
zig build test                     # core test suite
```

Produces `zig-out/bin/{HPCSV.exe, hpcs-lsp.exe, hpcs.exe}`. If Zig is not on `PATH`, call it
with its full path: `& "C:\tools\zig\zig.exe" build -Doptimize=ReleaseFast`.

<details>
<summary><b>Building the Zed extension (Windows)</b></summary>

```powershell
cd extension
cargo build --target wasm32-wasip2 --release
```

Zed compiles the extension itself on install. Two common pitfalls:

1. **`link.exe` not found** — Rust defaults to the MSVC toolchain, which requires Visual
   Studio. Switch to the GNU toolchain:

   ```powershell
   rustup default stable-x86_64-pc-windows-gnu
   rustup target add wasm32-wasip2
   ```

2. **Accented repository path** (e.g. `Dépots`) — MinGW cannot open such paths. Prepare an
   ASCII copy and install the dev extension from it:

   ```powershell
   .\scripts\prepare-dev-extension.ps1   # → C:\hpcs-extension
   ```

After installing Rust, **restart Zed** so it picks up `cargo`/`rustup`.

</details>

<details>
<summary><b>C linter (clangd)</b></summary>

On a machine without Visual Studio / Windows SDK, clangd finds neither `windows.h` nor the
project headers. Give it the same target as Zig through a compilation database:

```powershell
.\scripts\gen_compile_commands.ps1
# or: .\scripts\gen_compile_commands.ps1 -Zig C:\tools\zig\zig.exe
```

`compile_commands.json` (gitignored, machine-specific) carries the right
`--target=x86_64-w64-windows-gnu` and Zig's MinGW headers; `.clangd` forces the target for
files outside the database. Then reload clangd (`editor: restart language server`). A
`HPCSV: regenerate clangd database` task is provided in `.zed/tasks.json`.

</details>

---

## Author

[**Red**](https://github.com/RedTheFoxx)

---

## License

MIT — see [LICENSE](LICENSE).
