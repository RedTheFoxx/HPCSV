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

    const core = b.addStaticLibrary(.{
        .name = "hpcs",
        .target = target,
        .optimize = optimize,
    });
    core.addCSourceFiles(.{
        .files = &.{"core/src/hpcs.c"},
        .flags = &cflags,
    });
    core.addIncludePath(b.path("core/include"));
    core.linkLibC();
    b.installArtifact(core);

    const cli = b.addExecutable(.{
        .name = "hpcs",
        .target = target,
        .optimize = optimize,
    });
    cli.addCSourceFiles(.{
        .files = &.{"cli/src/main.c"},
        .flags = &cflags,
    });
    cli.addIncludePath(b.path("core/include"));
    cli.linkLibrary(core);
    cli.linkLibC();
    b.installArtifact(cli);

    const lsp = b.addExecutable(.{
        .name = "hpcs-lsp",
        .target = target,
        .optimize = optimize,
    });
    lsp.addCSourceFiles(.{
        .files = &.{"lsp/src/main.c"},
        .flags = &cflags,
    });
    lsp.addIncludePath(b.path("core/include"));
    lsp.linkLibrary(core);
    lsp.linkLibC();
    b.installArtifact(lsp);

    const tests = b.addExecutable(.{
        .name = "hpcs-tests",
        .target = target,
        .optimize = optimize,
    });
    tests.addCSourceFiles(.{
        .files = &.{"core/tests/test_core.c"},
        .flags = &cflags,
    });
    tests.addIncludePath(b.path("core/include"));
    tests.linkLibrary(core);
    tests.linkLibC();

    const run_tests = b.addRunArtifact(tests);
    const test_step = b.step("test", "Run the HPCSV core test suite");
    test_step.dependOn(&run_tests.step);

    const is_windows = target.result.os.tag == .windows;
    if (is_windows) {
        const app = b.addExecutable(.{
            .name = "HPCSV",
            .target = target,
            .optimize = optimize,
        });
        app.addCSourceFiles(.{
            .files = &.{ "app/src/main.c", "app/src/render.c" },
            .flags = &cflags,
        });
        app.addIncludePath(b.path("core/include"));
        app.addIncludePath(b.path("app"));
        app.linkLibrary(core);
        app.linkLibC();
        app.subsystem = .Windows;
        app.linkSystemLibrary("user32");
        app.linkSystemLibrary("gdi32");
        app.linkSystemLibrary("shell32");
        app.linkSystemLibrary("comctl32");
        app.linkSystemLibrary("comdlg32");
        app.linkSystemLibrary("ole32");
        b.installArtifact(app);
    }
}
