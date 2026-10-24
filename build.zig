const std = @import("std");

const cflags = [_][]const u8{
    "-std=c23",
    "-Wall",
    "-Wextra",
    "-Wno-unused-parameter",
    "-O2",
};

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const core_mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    core_mod.addCSourceFiles(.{
        .files = &.{"core/src/hpcs.c"},
        .flags = &cflags,
    });
    core_mod.addIncludePath(b.path("core/include"));
    const core = b.addLibrary(.{
        .name = "hpcs",
        .linkage = .static,
        .root_module = core_mod,
    });
    b.installArtifact(core);

    const cli_mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    cli_mod.addCSourceFiles(.{
        .files = &.{"cli/src/main.c"},
        .flags = &cflags,
    });
    cli_mod.addIncludePath(b.path("core/include"));
    cli_mod.linkLibrary(core);
    const cli = b.addExecutable(.{
        .name = "hpcs",
        .root_module = cli_mod,
    });
    b.installArtifact(cli);

    const lsp_mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    lsp_mod.addCSourceFiles(.{
        .files = &.{"lsp/src/main.c"},
        .flags = &cflags,
    });
    lsp_mod.addIncludePath(b.path("core/include"));
    lsp_mod.linkLibrary(core);
    const lsp = b.addExecutable(.{
        .name = "hpcs-lsp",
        .root_module = lsp_mod,
    });
    b.installArtifact(lsp);

    const tests_mod = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    tests_mod.addCSourceFiles(.{
        .files = &.{"core/tests/test_core.c"},
        .flags = &cflags,
    });
    tests_mod.addIncludePath(b.path("core/include"));
    tests_mod.linkLibrary(core);
    const tests = b.addExecutable(.{
        .name = "hpcs-tests",
        .root_module = tests_mod,
    });

    const run_tests = b.addRunArtifact(tests);
    const test_step = b.step("test", "Run the HPCSV core test suite");
    test_step.dependOn(&run_tests.step);

    const is_windows = target.result.os.tag == .windows;
    if (is_windows) {
        const app_mod = b.createModule(.{
            .target = target,
            .optimize = optimize,
            .link_libc = true,
        });
        app_mod.addCSourceFiles(.{
            .files = &.{ "app/src/main.c", "app/src/render.c" },
            .flags = &cflags,
        });
        app_mod.addIncludePath(b.path("core/include"));
        app_mod.addIncludePath(b.path("app"));
        app_mod.linkLibrary(core);
        app_mod.linkSystemLibrary("user32", .{});
        app_mod.linkSystemLibrary("gdi32", .{});
        app_mod.linkSystemLibrary("shell32", .{});
        app_mod.linkSystemLibrary("comctl32", .{});
        app_mod.linkSystemLibrary("comdlg32", .{});
        app_mod.linkSystemLibrary("ole32", .{});
        const app = b.addExecutable(.{
            .name = "HPCSV",
            .root_module = app_mod,
        });
        app.subsystem = .windows;
        b.installArtifact(app);
    }
}
