use zed_extension_api as zed;

struct HpcsExtension;

impl zed::Extension for HpcsExtension {
    fn new() -> Self {
        HpcsExtension
    }

    fn language_server_command(
        &mut self,
        _language_server_id: &zed::LanguageServerId,
        worktree: &zed::Worktree,
    ) -> zed::Result<zed::Command> {
        let command = worktree.which("hpcs-lsp").ok_or_else(|| {
            "hpcs-lsp was not found on PATH. Run `zig build` in the HPCSV repository, then add \
             `zig-out/bin` to PATH (or set HPCSV_BIN)."
                .to_string()
        })?;

        Ok(zed::Command {
            command,
            args: Vec::new(),
            env: Default::default(),
        })
    }
}

zed::register_extension!(HpcsExtension);
