#! /usr/bin/env python3
"""Keeps each WebAssembly runtime behind its interface: a runtime is reached only from its backend.

Status-line extensions run on a runtime-neutral interface (src/vtextension/wasm/WasmRuntime.hpp). Each
implementation of it lives in a directory of its own, src/vtextension/wasm/<runtime>/ (wamr/ for WAMR,
wasmtime/ for Wasmtime), and is built as its own target (vtextension_<runtime>), which only the
composition roots and the test executables link. This gate fails when that containment leaks, for every
backend in BACKENDS:

1. A file under src/ outside the backend's directory includes one of its runtime's own C headers (WAMR's
   wasm_export.h, wasm_c_api.h, lib_export.h, wasm_runtime.h; Wasmtime's wasmtime.h, wasm.h, wasi.h and
   wasmtime/...). No exception, tests included: a test of a runtime's C API belongs beside its backend.
2. src/vtextension/CMakeLists.txt or src/vthost/CMakeLists.txt mentions the runtime's link target
   (contour::wamr, contour::wasmtime). Neither library links a runtime; whoever composes the program does.
3. A file under src/ outside the backend's directory includes a header of that backend
   (vtextension/wasm/<runtime>/...). No exception either: a composition root or a runtime-agnostic test
   names vtextension/wasm/DefaultBackend.hpp instead, and what the tests need of the backend beyond the
   interface is declared neutrally (vtextension/wasm/testing/) and defined in the backend directory.

Every offending line is printed as path:line. `--self-test` checks the patterns against inline samples
and needs no tree. Python rather than shell, so that ctest runs it on every platform.
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SOURCE_ROOT = REPO_ROOT / "src"



@dataclass(frozen=True)
class Backend:
    """One runtime's backend: the only directory that may name the runtime."""

    name: str
    directory: Path
    c_header: re.Pattern[str]
    backend_header: re.Pattern[str]
    link: re.Pattern[str]


BACKENDS = (
    Backend(
        name="WAMR",
        directory=Path("src/vtextension/wasm/wamr"),
        c_header=re.compile(r'#include\s*[<"](wasm_export|wasm_c_api|lib_export|wasm_runtime)\.h[>"]'),
        backend_header=re.compile(r'#include\s*[<"](vtextension/)?wasm/wamr/'),
        link=re.compile(r"contour::wamr\b"),
    ),
    Backend(
        name="Wasmtime",
        directory=Path("src/vtextension/wasm/wasmtime"),
        c_header=re.compile(r'#include\s*[<"](wasmtime\.h|wasm\.h|wasi\.h|wasmtime/)'),
        backend_header=re.compile(r'#include\s*[<"](vtextension/)?wasm/wasmtime/'),
        link=re.compile(r"contour::wasmtime\b"),
    ),
)

WAMR, WASMTIME = BACKENDS

# The build files of the libraries that must not link the runtime themselves.
RUNTIME_FREE_BUILD_FILES = (Path("src/vtextension/CMakeLists.txt"), Path("src/vthost/CMakeLists.txt"))

SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp"}



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
        shown = relative.as_posix()
        for backend in BACKENDS:
            if is_within(relative, backend.directory):
                continue
            where = backend.directory.as_posix()
            for number in matching_lines(absolute, backend.c_header):
                violations.append(f"{shown}:{number}: includes a {backend.name} C header outside {where}/")
            for number in matching_lines(absolute, backend.backend_header):
                violations.append(
                    f"{shown}:{number}: includes a backend header outside {where}/; "
                    "include vtextension/wasm/DefaultBackend.hpp instead"
                )
    for relative in RUNTIME_FREE_BUILD_FILES:
        absolute = REPO_ROOT / relative
        if not absolute.is_file():
            continue
        for backend in BACKENDS:
            for number in matching_lines(absolute, backend.link):
                violations.append(
                    f"{relative.as_posix()}:{number}: links {backend.name}; only the backend target links it"
                )
    return violations


def self_test() -> int:
    """Checks the patterns and the backend-directory rule against inline samples."""
    cases: list[tuple[str, bool, bool]] = [
        # (description, actual, expected)
        ("angle-bracket WAMR header", bool(WAMR.c_header.search("#include <wasm_export.h>")), True),
        ("quoted WAMR header", bool(WAMR.c_header.search('#include "wasm_c_api.h"')), True),
        ("indented WAMR header", bool(WAMR.c_header.search("    #include <lib_export.h>")), True),
        ("wasm_runtime.h", bool(WAMR.c_header.search("#include<wasm_runtime.h>")), True),
        ("interface header is not WAMR", bool(WAMR.c_header.search("#include <vtextension/wasm/WasmRuntime.hpp>")), False),
        ("similar name is not WAMR", bool(WAMR.c_header.search("#include <my_wasm_export.h>")), False),
        ("WAMR named in a comment", bool(WAMR.c_header.search("// wraps wasm_export.h")), False),
        ("WAMR backend header", bool(WAMR.backend_header.search("#include <vtextension/wasm/wamr/WamrBackend.hpp>")), True),
        ("relative backend header", bool(WAMR.backend_header.search('#include "wasm/wamr/WamrRuntime.hpp"')), True),
        ("default backend header", bool(WAMR.backend_header.search("#include <vtextension/wasm/DefaultBackend.hpp>")), False),
        ("backend named in a comment", bool(WAMR.backend_header.search("// a backend (wasm/wamr/ for WAMR)")), False),
        ("link of contour::wamr", bool(WAMR.link.search("target_link_libraries(x PRIVATE contour::wamr)")), True),
        ("link of the runtime target", bool(WAMR.link.search("target_link_libraries(x vtextension_runtime)")), False),
        ("WAMR dir is exempt", is_within(Path("src/vtextension/wasm/wamr/WamrRuntime.hpp"), WAMR.directory), True),
        ("sibling dir is not exempt", is_within(Path("src/vtextension/wasm/WasmRuntime.hpp"), WAMR.directory), False),
        ("wasmtime.h", bool(WASMTIME.c_header.search("#include <wasmtime.h>")), True),
        ("wasm.h", bool(WASMTIME.c_header.search("#include <wasm.h>")), True),
        ("wasi.h", bool(WASMTIME.c_header.search('#include "wasi.h"')), True),
        ("a wasmtime/ header", bool(WASMTIME.c_header.search("#include <wasmtime/config.h>")), True),
        ("WAMR header is not Wasmtime", bool(WASMTIME.c_header.search("#include <wasm_export.h>")), False),
        ("similar name is not Wasmtime", bool(WASMTIME.c_header.search("#include <my_wasm.h>")), False),
        ("Wasmtime backend header", bool(WASMTIME.backend_header.search("#include <vtextension/wasm/wasmtime/WasmtimeBackend.hpp>")), True),
        ("WAMR backend header is not Wasmtime's", bool(WASMTIME.backend_header.search("#include <vtextension/wasm/wamr/WamrBackend.hpp>")), False),
        ("link of contour::wasmtime", bool(WASMTIME.link.search("target_link_libraries(x PRIVATE contour::wasmtime)")), True),
        ("contour::wasmtime is not contour::wamr", bool(WAMR.link.search("target_link_libraries(x contour::wasmtime)")), False),
        ("Wasmtime dir is exempt", is_within(Path("src/vtextension/wasm/wasmtime/WasmtimeModule.cpp"), WASMTIME.directory), True),
        ("WAMR dir is not Wasmtime's", is_within(Path("src/vtextension/wasm/wamr/WamrBackend.cpp"), WASMTIME.directory), False),
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
    print("check-wasm-runtime-containment: every WebAssembly runtime is contained.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
