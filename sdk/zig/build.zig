//! Builds sdk/zig/examples/hello-panel into a loadable .odmod.
//!
//!   zig build            -> zig-out/bin/mod.wasm
//!   zig build pack       -> examples/hello-panel/hello-panel-zig.odmod
//!
//! WRITTEN AGAINST THE ZIG 0.15.x BUILD API, AND NOT COMPILED — see README.md.
//! The build system is the part of Zig that churns most between releases. If
//! `zig build` fails here with something about `root_module`, you are on 0.14
//! or older: see the commented block in exampleExe() below, or just use
//! examples/hello-panel/build.sh, which only uses compiler flags.

const std = @import("std");

pub fn build(b: *std.Build) void {
    // A mod runs inside an interpreter with a fuel budget, so size and
    // predictability beat unrolled speed. ReleaseSmall also drops the safety
    // checks that would otherwise pull panic formatting into a 2 KB module.
    const optimize = b.standardOptimizeOption(.{ .preferred_optimize_mode = .ReleaseSmall });

    // Freestanding, not wasi: the host deliberately does not link WASI, so a
    // module importing fd_write would be refused at load.
    const target = b.resolveTargetQuery(.{
        .cpu_arch = .wasm32,
        .os_tag = .freestanding,
    });

    const gearbox = b.createModule(.{
        .root_source_file = b.path("gearbox.zig"),
        .target = target,
        .optimize = optimize,
    });

    const root = b.createModule(.{
        .root_source_file = b.path("examples/hello-panel/main.zig"),
        .target = target,
        .optimize = optimize,
    });
    root.addImport("gearbox", gearbox);

    const mod = b.addExecutable(.{
        .name = "mod",
        .root_module = root,
    });

    // Zig 0.14 and older take the module fields inline instead:
    //
    //   const mod = b.addExecutable(.{
    //       .name = "mod",
    //       .root_source_file = b.path("examples/hello-panel/main.zig"),
    //       .target = target,
    //       .optimize = optimize,
    //   });
    //   mod.root_module.addImport("gearbox", gearbox);

    // No _start. The host calls mod_load and the other mod_* exports directly
    // and never calls an entry point, so asking for one is a link error.
    mod.entry = .disabled;

    // Keeps the `export fn`s in the final module. Whether `export fn` alone is
    // enough on wasm-freestanding has varied by release; rdynamic is the form
    // that has worked throughout. build.sh instead names each export with
    // --export=, which is stricter — it fails the link if you typo a name.
    mod.rdynamic = true;

    b.installArtifact(mod);

    // --- packing ---------------------------------------------------------
    // pack_odmod.sh wants a directory holding MANIFEST.json and mod.wasm, so
    // the built module is copied next to the manifest first. POSIX only; it
    // shells out to cp, and the packer itself is bash + zip.

    const example = "examples/hello-panel";

    const copy = b.addSystemCommand(&.{"cp"});
    copy.addFileArg(mod.getEmittedBin());
    copy.addArg(b.pathFromRoot(example ++ "/mod.wasm"));

    const pack = b.addSystemCommand(&.{
        b.pathFromRoot("../../tools/pack_odmod.sh"),
        b.pathFromRoot(example),
        b.pathFromRoot(example ++ "/hello-panel-zig.odmod"),
    });
    pack.step.dependOn(&copy.step);

    const pack_step = b.step("pack", "Build mod.wasm and pack it into a .odmod");
    pack_step.dependOn(&pack.step);
}
