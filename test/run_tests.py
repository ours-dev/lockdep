#!/usr/bin/env python3
"""Build and run lockdep's host tests.

lockdep is header-only and has no GN test target yet, so this drives host
compilers directly:

  * smoke.cpp at every feature level -- the logic, against fake locks;
  * threaded.cpp at the validation level -- real threads, one lock context
    each, recording into the shared graph at once -- plain, and under
    ThreadSanitizer and AddressSanitizer/UndefinedBehaviorSanitizer.

Both are built twice. Once with the host compiler, which on macOS is the only
one that ships the sanitizer runtimes. And once with the ours tree's own
clang, under the kernel's language level and warning flags, because that is
the compiler and those are the flags every lockdep header is really built
with: a test run only under the host compiler once let a
-Wunused-but-set-global error and a name-lookup error in a discarded
if-constexpr branch through, both of which the tree's clang rejects.

The headers are not freestanding of the ours tree -- they use the project's
integer aliases from the force-included prelude, ustl, and the thread-safety
annotation macros in system/public -- so the ours checkout is found by walking
up from this file, or named with --ours.

    test/run_tests.py [--ours DIR] [--suite smoke|threaded] [--compiler host|tree]
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

HERE = Path(__file__).resolve().parent
LIBRARY = HERE.parent

LEVELS = {
    "disabled": [],
    "metadata-only": ["-DCONFIG_LOCK_DEP=1", "-DLOCK_DEP_ENABLED_FEATURE_LEVEL=1"],
    "validation": ["-DCONFIG_LOCK_DEP=1", "-DLOCK_DEP_ENABLED_FEATURE_LEVEL=2"],
}

SANITIZERS = {
    "thread-sanitizer": ["-O1", "-g", "-fsanitize=thread"],
    "address-undefined": [
        "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
    ],
}

# The warning flags the kernel compiles every translation unit with, taken from
# build/config. -Wthread-safety matters most: Guard is a scoped capability, and
# only this flag makes the annotations on it mean anything.
KERNEL_WARNINGS = [
    "-Wall",
    "-Wextra",
    "-Wconversion",
    "-Wimplicit-fallthrough",
    "-Wwrite-strings",
    "-Wno-sign-conversion",
    "-Wno-unused-parameter",
    "-Wformat=2",
    "-Wmissing-declarations",
    "-Wshadow",
    "-Wvla",
    "-Wthread-safety",
    "-Wno-thread-safety-attributes",
    "-Werror",
]


@dataclass
class Compiler:
    name: str
    cxx: str
    flags: list[str] = field(default_factory=list)
    sanitizers: bool = False


def find_ours(start: Path) -> Path | None:
    """The nearest ancestor that looks like an ours checkout.

    A lockdep worktree usually sits inside one (under .claude/worktrees or at
    system/lib/lockdep), so walking up finds the tree whose ustl and prelude
    the kernel itself builds against.
    """
    for candidate in [start, *start.parents]:
        if (candidate / "kernel/include/ours/prelude.hpp").is_file() and (
            candidate / "system/lib/ustl/include"
        ).is_dir():
            return candidate
    return None


def sdk_flags() -> list[str]:
    """A non-Apple clang on macOS finds no C++ standard library on its own."""
    if sys.platform != "darwin":
        return []
    sdk = subprocess.run(["xcrun", "--show-sdk-path"], capture_output=True, text=True)
    if sdk.returncode != 0:
        return []
    return ["-isysroot", sdk.stdout.strip()]


def compilers(ours: Path, host_cxx: str) -> dict[str, Compiler]:
    found = {
        "host": Compiler("host", host_cxx, ["-std=c++23"], sanitizers=True),
    }
    tree = ours / "prebuilt/clang/llvm-23/bin/clang++"
    if tree.is_file():
        found["tree"] = Compiler("tree", str(tree), ["-std=c++2c", *sdk_flags()])
    return found


def include_flags(ours: Path) -> list[str]:
    return [
        f"-I{LIBRARY / 'include'}",
        f"-I{ours / 'system/lib/ustl/include'}",
        f"-I{ours / 'system/public'}",
        "-include",
        str(ours / "kernel/include/ours/prelude.hpp"),
    ]


def build_and_run(compiler: Compiler, flags: list[str], source: Path, name: str,
                  scratch: Path) -> bool:
    label = f"{compiler.name}/{name}"
    binary = scratch / label.replace("/", ".")
    print(f"=== {label} ===", flush=True)
    compiled = subprocess.run([compiler.cxx, *compiler.flags, *flags, str(source),
                               "-o", str(binary)])
    if compiled.returncode != 0:
        print(f"COMPILE FAILED: {label}")
        return False
    ran = subprocess.run([str(binary)])
    if ran.returncode != 0:
        print(f"RUNTIME FAILED: {label} (exit {ran.returncode})")
        return False
    print(f"PASS: {label}")
    return True


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--ours", type=Path, help="ours checkout providing ustl and the prelude")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"), help="host C++ compiler")
    parser.add_argument("--suite", choices=["smoke", "threaded"], action="append",
                        help="run only this suite (repeatable)")
    parser.add_argument("--compiler", choices=["host", "tree"], action="append",
                        help="run only with this compiler (repeatable)")
    args = parser.parse_args(argv)

    ours = args.ours or find_ours(LIBRARY)
    if ours is None:
        print("cannot find an ours checkout above this library; pass --ours", file=sys.stderr)
        return 2
    ours = ours.resolve()

    available = compilers(ours, args.cxx)
    wanted = args.compiler or list(available)
    missing = [name for name in wanted if name not in available]
    if missing:
        print(f"compiler not available: {', '.join(missing)}", file=sys.stderr)
        return 2

    suites = args.suite or ["smoke", "threaded"]
    common = [*KERNEL_WARNINGS, *include_flags(ours)]
    scratch = Path(tempfile.mkdtemp(prefix="lockdep-tests."))
    ok = True
    try:
        for compiler in (available[name] for name in wanted):
            if "smoke" in suites:
                for level, defines in LEVELS.items():
                    ok &= build_and_run(compiler, [*common, *defines], HERE / "smoke.cpp",
                                        f"smoke.{level}", scratch)
            if "threaded" in suites:
                variants = {"plain": ["-O2"]}
                if compiler.sanitizers:
                    variants.update(SANITIZERS)
                for variant, extra in variants.items():
                    ok &= build_and_run(compiler, [*common, *LEVELS["validation"], *extra],
                                        HERE / "threaded.cpp", f"threaded.{variant}", scratch)
    finally:
        shutil.rmtree(scratch, ignore_errors=True)

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
