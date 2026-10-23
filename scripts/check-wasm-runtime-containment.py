#! /usr/bin/env python3
"""Keeps the WebAssembly runtime behind its interface: WAMR is reached only from its backend.

Status-line extensions run on a runtime-neutral interface (src/vtextension/wasm/WasmRuntime.hpp). The
one implementation of it, the WAMR backend, lives in src/vtextension/wasm/wamr/ and is built as its own
target (vtextension_wamr), which only the composition roots and the test executables link, through
vtextension_runtime. This gate fails when that containment leaks:

1. A file under src/ outside src/vtextension/wasm/wamr/ includes one of WAMR's own C headers
   (wasm_export.h, wasm_c_api.h, lib_export.h, wasm_runtime.h). No exception, tests included: a test of
   WAMR's C API belongs beside the backend.
2. src/vtextension/CMakeLists.txt or src/vthost/CMakeLists.txt mentions contour::wamr. Neither library
   links the runtime; whoever composes the program does.
3. A file under src/ outside src/vtextension/wasm/wamr/ includes a backend header
   (vtextension/wasm/wamr/...). No exception either: a composition root or a runtime-agnostic test names
   vtextension/wasm/DefaultBackend.hpp instead, and what the tests need of the backend beyond the
   interface is declared neutrally (vtextension/wasm/testing/) and defined in the backend directory.

Every offending line is printed as path:line. `--self-test` checks the patterns against inline samples
and needs no tree. Python rather than shell, so that ctest runs it on every platform.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SOURCE_ROOT = REPO_ROOT / "src"

# The backend directory: the only place that may name WAMR.
BACKEND_DIR = Path("src/vtextension/wasm/wamr")

# The build files of the libraries that must not link the runtime themselves.
RUNTIME_FREE_BUILD_FILES = (Path("src/vtextension/CMakeLists.txt"), Path("src/vthost/CMakeLists.txt"))

SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp"}

WAMR_C_HEADER = re.compile(r'#include\s*[<"](wasm_export|wasm_c_api|lib_export|wasm_runtime)\.h[>"]')
BACKEND_HEADER = re.compile(r'#include\s*[<"](vtextension/)?wasm/wamr/')
WAMR_LINK = re.compile(r"contour::wamr")


def is_within(path: Path, directory: Path) -> bool:
    """Whether the repository-relative `path` lies in the repository-relative `directory`."""
    return path == directory or directory in path.parents


def matching_lines(path: Path, pattern: re.Pattern[str]) -> list[int]:
    """The 1-based numbers of the lines of `path` that `pattern` finds something in."""
    text = path.read_text(encoding="utf-8", errors="replace")
    return [number for number, line in enumerate(text.splitlines(), start=1) if pattern.search(line)]


def scan() -> list[str]:
    """Every violation in the tree, as `path:line: reason`."""
    violations: list[str] = []
    for absolute in sorted(SOURCE_ROOT.rglob("*")):
        if not absolute.is_file() or absolute.suffix not in SOURCE_SUFFIXES:
            continue
        relative = absolute.relative_to(REPO_ROOT)
        if is_within(relative, BACKEND_DIR):
            continue
        shown = relative.as_posix()
        for number in matching_lines(absolute, WAMR_C_HEADER):
            violations.append(f"{shown}:{number}: includes a WAMR C header outside {BACKEND_DIR.as_posix()}/")
        for number in matching_lines(absolute, BACKEND_HEADER):
            violations.append(
                f"{shown}:{number}: includes a backend header outside {BACKEND_DIR.as_posix()}/; "
                "include vtextension/wasm/DefaultBackend.hpp instead"
            )
    for relative in RUNTIME_FREE_BUILD_FILES:
        absolute = REPO_ROOT / relative
        if absolute.is_file():
            for number in matching_lines(absolute, WAMR_LINK):
                violations.append(
                    f"{relative.as_posix()}:{number}: mentions contour::wamr; only the backend target links it"
                )
    return violations


def self_test() -> int:
    """Checks the patterns and the backend-directory rule against inline samples."""
    cases: list[tuple[str, bool, bool]] = [
        # (description, actual, expected)
        ("angle-bracket WAMR header", bool(WAMR_C_HEADER.search("#include <wasm_export.h>")), True),
        ("quoted WAMR header", bool(WAMR_C_HEADER.search('#include "wasm_c_api.h"')), True),
        ("indented WAMR header", bool(WAMR_C_HEADER.search("    #include <lib_export.h>")), True),
        ("wasm_runtime.h", bool(WAMR_C_HEADER.search("#include<wasm_runtime.h>")), True),
        ("interface header is not WAMR", bool(WAMR_C_HEADER.search("#include <vtextension/wasm/WasmRuntime.hpp>")), False),
        ("similar name is not WAMR", bool(WAMR_C_HEADER.search("#include <my_wasm_export.h>")), False),
        ("WAMR named in a comment", bool(WAMR_C_HEADER.search("// wraps wasm_export.h")), False),
        ("backend header", bool(BACKEND_HEADER.search("#include <vtextension/wasm/wamr/WamrBackend.hpp>")), True),
        ("relative backend header", bool(BACKEND_HEADER.search('#include "wasm/wamr/WamrRuntime.hpp"')), True),
        ("default backend header", bool(BACKEND_HEADER.search("#include <vtextension/wasm/DefaultBackend.hpp>")), False),
        ("backend named in a comment", bool(BACKEND_HEADER.search("// a backend (wasm/wamr/ for WAMR)")), False),
        ("link of contour::wamr", bool(WAMR_LINK.search("target_link_libraries(x PRIVATE contour::wamr)")), True),
        ("link of the runtime target", bool(WAMR_LINK.search("target_link_libraries(x vtextension_runtime)")), False),
        ("backend dir is exempt", is_within(Path("src/vtextension/wasm/wamr/WamrRuntime.hpp"), BACKEND_DIR), True),
        ("sibling dir is not exempt", is_within(Path("src/vtextension/wasm/WasmRuntime.hpp"), BACKEND_DIR), False),
    ]
    failures = [description for description, actual, expected in cases if actual != expected]
    for description in failures:
        print(f"check-wasm-runtime-containment self-test: FAILED: {description}", file=sys.stderr)
    if failures:
        print(f"check-wasm-runtime-containment self-test: {len(failures)} of {len(cases)} cases failed.", file=sys.stderr)
        return 1
    print(f"check-wasm-runtime-containment self-test: {len(cases)} cases OK.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true", help="check the patterns against inline samples")
    arguments = parser.parse_args()
    if arguments.self_test:
        return self_test()

    violations = scan()
    for violation in violations:
        print(violation, file=sys.stderr)
    if violations:
        print(f"check-wasm-runtime-containment: {len(violations)} violation(s).", file=sys.stderr)
        return 1
    print("check-wasm-runtime-containment: the WebAssembly runtime is contained.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
