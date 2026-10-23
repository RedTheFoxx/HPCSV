use zed_extension_api::{self as zed, Command, LanguageServerId, Result, Worktree};

struct HpcsExtension;

impl HpcsExtension {
    /// Executable name of the language server, including the platform suffix.
    fn lsp_binary_name() -> &'static str {
        match zed::current_platform().0 {
            zed::Os::Windows => "hpcs-lsp.exe",
            _ => "hpcs-lsp",
        }
    }

    /// Reads an environment variable from the worktree's shell environment.
    fn env_var(worktree: &Worktree, name: &str) -> Option<String> {
        worktree
            .shell_env()
            .into_iter()
            .find_map(|(key, value)| (key == name && !value.is_empty()).then_some(value))
    }

    /// Returns the parent directory of a path, without relying on
    /// `std::path` (which uses WASI separators and would mishandle
    /// Windows `\`).
    fn parent_dir(path: &str) -> Option<&str> {
        let index = path.rfind(['/', '\\'])?;
        Some(&path[..index])
    }

    /// Joins a directory and a file name using the host OS separator.
    fn join(dir: &str, name: &str) -> String {
        let separator = match zed::current_platform().0 {
            zed::Os::Windows => '\\',
            _ => '/',
        };
        let dir = dir.trim_end_matches(['/', '\\']);
        format!("{dir}{separator}{name}")
    }

    /// Resolves the `hpcs-lsp` executable.
    ///
    /// Resolution order:
    /// 1. `HPCSV_LSP`, an explicit path to the server;
    /// 2. `HPCSV_BIN`'s directory, where `hpcs-lsp` sits next to `HPCSV.exe`;
    /// 3. `$PATH`, via `Worktree::which`;
    /// 4. the local `zig build` output of the HPCSV repository, when the
    ///    opened worktree is that repository.
    fn lsp_path(worktree: &Worktree) -> Result<String> {
        let binary = Self::lsp_binary_name();

        // 1. Explicit override.
        if let Some(path) = Self::env_var(worktree, "HPCSV_LSP") {
            return Ok(path);
        }

        // 2. Next to HPCSV.exe, as pointed to by HPCSV_BIN.
        if let Some(exe) = Self::env_var(worktree, "HPCSV_BIN") {
            if let Some(dir) = Self::parent_dir(&exe) {
                return Ok(Self::join(dir, binary));
            }
        }

        // 3. On the PATH.
        if let Some(found) = worktree.which("hpcs-lsp") {
            return Ok(found);
        }
        if let Some(found) = worktree.which(binary) {
            return Ok(found);
        }

        // 4. A local build inside the opened worktree. The `build.zig` marker
        //    ensures we only do this for the HPCSV repository itself, so other
        //    projects never resolve to a non-existent path.
        if let Ok(build_zig) = worktree.read_text_file("build.zig") {
            if build_zig.contains("hpcs-lsp") {
                let root = worktree.root_path();
                let candidate =
                    Self::join(&Self::join(&Self::join(&root, "zig-out"), "bin"), binary);
                return Ok(candidate);
            }
        }

        Err(format!(
            "hpcs-lsp was not found. Run `zig build` in the HPCSV repository so that \
             `zig-out/bin/{binary}` exists, add `zig-out/bin` to PATH, set `HPCSV_LSP` to \
             the full path of the language server, or set `HPCSV_BIN` to the directory \
             containing HPCSV.exe."
        ))
    }
}

impl zed::Extension for HpcsExtension {
    fn new() -> Self {
        HpcsExtension
    }

    fn language_server_command(
        &mut self,
        _language_server_id: &LanguageServerId,
        worktree: &Worktree,
    ) -> Result<Command> {
        Ok(Command {
            command: Self::lsp_path(worktree)?,
            args: Vec::new(),
            env: Default::default(),
        })
    }
}

zed::register_extension!(HpcsExtension);
