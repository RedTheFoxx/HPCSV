# HPCSV for Zed

CSV/TSV language support for [HPCSV](https://github.com/hpcs/hpcs), backed by the pure C
`hpcs-lsp` language server.

## What you get

- CSV and TSV syntax highlighting;
- diagnostics for rows whose field count differs from the header, and for unterminated
  quoted fields;
- hover a cell to see its address, column name, type and a summary of the row;
- the **HPCSV: open in spreadsheet** code action, which launches the native HPCSV
  spreadsheet on the current file.

## Requirement

`hpcs-lsp` (and `HPCSV`, for the code action) must be available:

```sh
zig build -Doptimize=ReleaseFast
# then put zig-out/bin on PATH, or set HPCSV_BIN to the full path of HPCSV.exe
```

The extension finds `hpcs-lsp` on its own: `HPCSV_LSP` (full path to the server), the
directory of `HPCSV_BIN`, `$PATH`, then the `zig-out/bin` of the HPCSV repository when it
is the opened worktree. The language server looks for `HPCSV.exe` next to itself, then
`HPCSV_BIN`, then `PATH`.

## Why a native app?

Zed's extension API does not allow custom UI. The CSV tab stays the source of truth; the
native spreadsheet edits the same file and the two stay in sync. See the
[project README](https://github.com/hpcs/hpcs) for the full rationale.

## License

MIT
