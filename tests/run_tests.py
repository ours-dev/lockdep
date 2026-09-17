#!/usr/bin/env python3
"""Build and run lockdep's host tests.

lockdep is header-only and has no GN test target yet, so this drives a host
compiler directly:

  * smoke.cpp at every feature level -- the logic, against fake locks;
  * threaded.cpp at the validation level, plain and under ThreadSanitizer and
    AddressSanitizer/UndefinedBehaviorSanitizer -- real threads, one lock
    context each, recording into the shared graph at once.

The headers are not freestanding of the ours tree: they use the project's
integer aliases from the force-included prelude, ustl, and the thread-safety
annotation macros in system/public. The ours checkout is found by walking up
from this file, or named with --ours.

    tests/run_tests.py [--ours DIR] [--cxx COMPILER] [--suite smoke|threaded]
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
LIBRARY = HERE.parent

LEVELS = {
    "disabled": [],
    "metadata-only": ["-DCONFIG_LOCK_DEP=1", "-DLOCK_DEP_ENABLED_FEATURE_LEVEL=1"],
    "validation": ["-DCONFIG_LOCK_DEP=1", "-DLOCK_DEP_ENABLED_FEATURE_LEVEL=2"],
}

SANITIZERS = {
    "plain": ["-O2"],
    "thread-sanitizer": ["-O1", "-g", "-fsanitize=thread"],
    "address-undefined": [
        "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
    ],
}


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


def base_flags(ours: Path) -> list[str]:
    return [
        "-std=c++23",
        "-Wall",
        "-Wextra",
        "-Wshadow",
        "-Werror",
        f"-I{LIBRARY / 'include'}",
        f"-I{ours / 'system/lib/ustl/include'}",
        f"-I{ours / 'system/public'}",
        "-include",
        str(ours / "kernel/include/ours/prelude.hpp"),
    ]


def build_and_run(cxx: str, flags: list[str], source: Path, name: str, scratch: Path) -> bool:
    binary = scratch / name
    print(f"=== {name} ===", flush=True)
    compiled = subprocess.run([cxx, *flags, str(source), "-o", str(binary)])
    if compiled.returncode != 0:
        print(f"COMPILE FAILED: {name}")
        return False
    ran = subprocess.run([str(binary)])
    if ran.returncode != 0:
        print(f"RUNTIME FAILED: {name} (exit {ran.returncode})")
        return False
    print(f"PASS: {name}")
    return True


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--ours", type=Path, help="ours checkout providing ustl and the prelude")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"), help="host C++ compiler")
    parser.add_argument("--suite", choices=["smoke", "threaded"], action="append",
                        help="run only this suite (repeatable)")
    args = parser.parse_args(argv)

    ours = args.ours or find_ours(LIBRARY)
    if ours is None:
        print("cannot find an ours checkout above this library; pass --ours", file=sys.stderr)
        return 2

    suites = args.suite or ["smoke", "threaded"]
    flags = base_flags(ours.resolve())
    scratch = Path(tempfile.mkdtemp(prefix="lockdep-tests."))
    ok = True
    try:
        if "smoke" in suites:
            for level, defines in LEVELS.items():
                ok &= build_and_run(args.cxx, [*flags, *defines], HERE / "smoke.cpp",
                                    f"smoke.{level}", scratch)
        if "threaded" in suites:
            for variant, extra in SANITIZERS.items():
                ok &= build_and_run(args.cxx, [*flags, *LEVELS["validation"], *extra],
                                    HERE / "threaded.cpp", f"threaded.{variant}", scratch)
    finally:
        shutil.rmtree(scratch, ignore_errors=True)

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
