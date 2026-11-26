#! /usr/bin/env python3
"""Keeps each WebAssembly runtime behind its interface: a runtime is reached only from its backend.

Status-line extensions run on a runtime-neutral interface (src/vtextension/wasm/WasmRuntime.hpp). Each
implementation of it lives in a directory of its own, src/vtextension/wasm/<runtime>/ (wamr/ for WAMR,
wasmtime/ for Wasmtime), and is built as its own target (vtextension_<runtime>), which only the
composition roots and the test executables link. This gate fails when that containment leaks, for every
backend in BACKENDS:

1. A file under src/ outside the backend's directory includes one of its runtime's own C headers (WAMR's
   wasm_export.h, wasm_c_api.h, lib_export.h, wasm_runtime.h; Wasmtime's wasmtime.h, wasm.h, wasi.h, the
   C++ API's wasmtime.hh and wasm.hh, and wasmtime/...). No exception, tests included: a test of a runtime's C API belongs beside its backend.
2. src/vtextension/CMakeLists.txt or src/vthost/CMakeLists.txt mentions the runtime's link target
   (contour::wamr, contour::wasmtime). Neither library links a runtime; whoever composes the program does.
3. A file under src/ outside the backend's directory includes a header of that backend
   (vtextension/wasm/<runtime>/...). No exception either: a composition root or a runtime-agnostic test
   names vtextension/wasm/DefaultBackend.hpp instead, and what the tests need of the backend beyond the
   interface is declared neutrally (vtextension/wasm/testing/) and defined in the backend directory.
4. An executable links more than one runtime, directly or through the libraries it links. Every runtime
   defines the standard wasm.h C API (wasm_byte_vec_new, ...): MSVC's linker refuses the second definition
   (LNK2005), and an ELF linker quietly binds every caller to whichever runtime it met first. The link graph
   is read from the build files under src/ and cmake/ with every condition taken, so that a pair only one
   platform builds is caught on all of them. A target made in a `foreach(<var> [IN ITEMS] ...)` is read once
   per item. A library named through a variable (vtextension_runtime's vtextension_${CONTOUR_WASM_RUNTIME})
   is one choice, never both: alone it is fine, but beside a runtime named outright it may pick the other,
   so that pair is reported. A variable `set()` once to one word in the same file is read as that word.

Every offending line is printed as path:line. `--self-test` checks the patterns against inline samples
and needs no tree. Python rather than shell, so that ctest runs it on every platform.
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
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
        c_header=re.compile(r'#include\s*[<"]((wasmtime|wasm|wasi)\.hh?[>"]|wasmtime/)'),
        backend_header=re.compile(r'#include\s*[<"](vtextension/)?wasm/wasmtime/'),
        link=re.compile(r"contour::wasmtime\b"),
    ),
)

WAMR, WASMTIME = BACKENDS

# The build files of the libraries that must not link the runtime themselves.
RUNTIME_FREE_BUILD_FILES = (Path("src/vtextension/CMakeLists.txt"), Path("src/vthost/CMakeLists.txt"))

SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp"}

# Where the link graph of rule 4 is read from: the first-party build files.
BUILD_FILE_ROOTS = (REPO_ROOT / "src", REPO_ROOT / "cmake")

# The commands rule 4 reads, with their arguments; one level of nested parentheses is enough for this tree.
BUILD_COMMAND = re.compile(
    r"\b(add_executable|qt_add_executable|target_link_libraries)\s*\(((?:[^()]|\([^()]*\))*)\)", re.IGNORECASE
)

# The scope and keyword arguments of target_link_libraries, which name no library.
LINK_KEYWORDS = {"PUBLIC", "PRIVATE", "INTERFACE", "LINK_PUBLIC", "LINK_PRIVATE", "LINK_INTERFACE_LIBRARIES"}

# A foreach() or endforeach(), whose loop variable rule 4 substitutes into the commands of its body.
FOREACH_COMMAND = re.compile(r"\b(foreach|endforeach)\s*\(([^()]*)\)", re.IGNORECASE)

# A set() of one variable to one word, which rule 4 reads as that word; a list or a cache entry is not.
SET_COMMAND = re.compile(r"\bset\s*\(\s*([A-Za-z0-9_]+)\s+([^\s()\"$]*(?:\$\{[A-Za-z0-9_]+\}[^\s()\"$]*)*)\s*\)", re.IGNORECASE)

# A variable reference in a command argument.
VARIABLE = re.compile(r"\$\{([A-Za-z0-9_]+)\}")



def is_within(path: Path, directory: Path) -> bool:
    """Whether the repository-relative `path` lies in the repository-relative `directory`."""
    return path == directory or directory in path.parents


def matching_lines(path: Path, pattern: re.Pattern[str]) -> list[int]:
    """The 1-based numbers of the lines of `path` that `pattern` finds something in."""
    text = path.read_text(encoding="utf-8", errors="replace")
    return [number for number, line in enumerate(text.splitlines(), start=1) if pattern.search(line)]


def strip_comments(text: str) -> str:
    """`text` with each line's comment removed, a `#` inside a quoted string kept."""
    lines: list[str] = []
    for line in text.splitlines():
        quoted = False
        kept = line
        for index, character in enumerate(line):
            if character == '"':
                quoted = not quoted
            elif character == "#" and not quoted:
                kept = line[:index]
                break
        lines.append(kept)
    return "\n".join(lines)


@dataclass(frozen=True)
class Loop:
    """A foreach() body: where it spans in the text, and the values of its variable, if they are written out."""

    start: int
    end: int
    variable: str
    items: tuple[str, ...] | None


def loops_of(text: str) -> list[Loop]:
    """The foreach() bodies of `text`; `items` is None for a loop over a list, a range or zipped lists."""
    found: list[Loop] = []
    open_loops: list[tuple[int, str, tuple[str, ...] | None]] = []
    for match in FOREACH_COMMAND.finditer(text):
        if match.group(1).lower() == "endforeach":
            if open_loops:
                start, variable, items = open_loops.pop()
                found.append(Loop(start, match.start(), variable, items))
            continue
        words = match.group(2).split()
        if not words:
            continue
        rest = words[1:]
        items: tuple[str, ...] | None = None
        if rest[:2] == ["IN", "ITEMS"]:
            items = tuple(rest[2:])
        elif not rest or rest[0] not in {"IN", "RANGE"}:
            items = tuple(rest)
        open_loops.append((match.end(), words[0], items))
    return found


def single_word_variables(text: str) -> dict[str, str]:
    """The variables `text` set()s exactly once, each to one word."""
    values: dict[str, list[str]] = {}
    for match in SET_COMMAND.finditer(text):
        values.setdefault(match.group(1), []).append(match.group(2))
    return {name: words[0] for name, words in values.items() if len(words) == 1 and words[0]}


def substitute(argument: str, values: dict[str, str]) -> str:
    """`argument` with every variable that `values` knows replaced by its value."""
    return VARIABLE.sub(lambda match: values.get(match.group(1), match.group(0)), argument)


def runtime_choices(argument: str) -> set[str]:
    """The libraries a link argument naming a runtime through a variable may be: one per backend's name."""
    if argument.startswith("$<") or not VARIABLE.search(argument):
        return set()
    return {VARIABLE.sub(backend.name.lower(), argument) for backend in BACKENDS}


@dataclass
class LinkGraph:
    """The executables of the build and what each target links, every condition taken."""

    executables: dict[str, str]
    links: dict[str, set[str]]
    # Per target, each of its link arguments that names a library through a variable: what it may be.
    choices: dict[str, list[set[str]]] = field(default_factory=dict)

    def read(self, text: str, shown: str) -> None:
        """Adds the executables and links that the build file `shown`, whose text is `text`, declares."""
        text = strip_comments(text)
        variables = single_word_variables(text)
        loops = loops_of(text)
        for match in BUILD_COMMAND.finditer(text):
            enclosing = [loop for loop in loops if loop.start <= match.start() < loop.end]
            for values in loop_values(enclosing, variables):
                self.read_command(match, text, shown, values)

    def read_command(self, match: re.Match[str], text: str, shown: str, values: dict[str, str]) -> None:
        """Adds what the one command `match` declares, its variables taken from `values`."""
        command = match.group(1).lower()
        arguments = [substitute(argument, values) for argument in match.group(2).split()]
        if not arguments:
            return
        target = arguments[0]
        if command == "target_link_libraries":
            libraries = [argument for argument in arguments[1:] if argument not in LINK_KEYWORDS]
            self.links.setdefault(target, set()).update(
                library for library in libraries if "$" not in library
            )
            self.choices.setdefault(target, []).extend(
                choice for choice in map(runtime_choices, libraries) if choice
            )
        else:
            line = text.count("\n", 0, match.start()) + 1
            self.executables.setdefault(target, f"{shown}:{line}")

    def reached(self, target: str) -> set[str]:
        """Every target `target` links, directly or through another; a library chosen by a variable is not followed."""
        seen: set[str] = set()
        pending = [target]
        while pending:
            for linked in self.links.get(pending.pop(), set()):
                if linked not in seen:
                    seen.add(linked)
                    pending.append(linked)
        return seen

    def runtimes(self, targets: set[str]) -> set[str]:
        """The names of the runtimes among `targets`."""
        return {backend.name for backend in BACKENDS if any(backend.link.fullmatch(name) for name in targets)}

    def violations(self) -> list[str]:
        """Each executable that links, or may link, more than one runtime, as `path:line: reason`."""
        found: list[str] = []
        for executable, where in sorted(self.executables.items()):
            reached = self.reached(executable) | {executable}
            named = self.runtimes(reached)
            chosen: set[str] = set()
            for target in reached:
                for choice in self.choices.get(target, []):
                    chosen |= {runtime for library in choice for runtime in self.runtimes(self.reached(library))}
            if len(named) > 1:
                found.append(
                    f"{where}: {executable} links {' and '.join(sorted(named))}; "
                    "each defines the wasm.h C API, so one executable links one runtime"
                )
            elif named and chosen - named:
                found.append(
                    f"{where}: {executable} links {' and '.join(sorted(named))} and a runtime chosen by a "
                    f"variable, which may be {' or '.join(sorted(chosen - named))}; one executable links one runtime"
                )
        return found


def loop_values(loops: list[Loop], variables: dict[str, str]) -> list[dict[str, str]]:
    """Every assignment of the written-out loop variables of `loops`, each on top of `variables`."""
    assignments: list[dict[str, str]] = [dict(variables)]
    for loop in loops:
        if loop.items is None:
            continue
        assignments = [{**assignment, loop.variable: item} for assignment in assignments for item in loop.items]
    return assignments


def read_link_graph() -> LinkGraph:
    """The link graph of the tree's first-party build files."""
    graph = LinkGraph(executables={}, links={})
    for root in BUILD_FILE_ROOTS:
        for absolute in sorted(root.rglob("*")):
            if absolute.is_file() and (absolute.name == "CMakeLists.txt" or absolute.suffix == ".cmake"):
                text = absolute.read_text(encoding="utf-8", errors="replace")
                graph.read(text, absolute.relative_to(REPO_ROOT).as_posix())
    return graph


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
    violations.extend(read_link_graph().violations())
    return violations


def link_graph_cases() -> list[tuple[str, bool, bool]]:
    """Rule 4 against an inline build: which executables it reports as linking both runtimes."""
    graph = LinkGraph(executables={}, links={})
    graph.read(
        """
add_library(engine_a STATIC a.cpp)
target_link_libraries(engine_a PUBLIC core PRIVATE contour::wamr)
add_library(engine_b STATIC b.cpp)
target_link_libraries(engine_b PRIVATE contour::wasmtime)
add_library(engine_b_probe STATIC probe.cpp)
target_link_libraries(engine_b_probe PUBLIC engine_b)
add_executable(direct main.cpp)
target_link_libraries(direct PRIVATE contour::wamr contour::wasmtime)
add_executable(transitive main.cpp)
target_link_libraries(transitive PRIVATE engine_a)
if(SOME_PLATFORM)
    target_link_libraries(transitive PRIVATE
        engine_b_probe)
endif()
add_executable(one_engine main.cpp)
target_link_libraries(one_engine PRIVATE engine_a)
add_library(engine_wamr STATIC w.cpp)
target_link_libraries(engine_wamr PRIVATE contour::wamr)
add_library(engine_wasmtime STATIC t.cpp)
target_link_libraries(engine_wasmtime PRIVATE contour::wasmtime)
add_library(engine_chosen INTERFACE)
target_link_libraries(engine_chosen INTERFACE engine_${RUNTIME})
add_executable(chosen main.cpp)
target_link_libraries(chosen PRIVATE engine_${RUNTIME})
add_executable(chosen_and_named main.cpp)
target_link_libraries(chosen_and_named PRIVATE engine_chosen engine_wamr)
set(_helper_engine engine_${RUNTIME})
add_executable(chosen_through_set main.cpp)
target_link_libraries(chosen_through_set PRIVATE ${_helper_engine} engine_wasmtime)
foreach(target IN ITEMS looped looped_heavy)
    target_link_libraries(${target} PRIVATE engine_a)
endforeach()
add_executable(looped main.cpp)
add_executable(looped_heavy main.cpp)
target_link_libraries(looped_heavy PRIVATE engine_b)
foreach(target plain_looped)
    add_executable(${target} main.cpp)
    target_link_libraries(${target} PRIVATE engine_a engine_b)
endforeach()
add_executable(commented main.cpp)
target_link_libraries(commented PRIVATE engine_a) # not engine_b
qt_add_executable(gui)
target_link_libraries(gui PRIVATE engine_a "$<TARGET_OBJECTS:engine_b>")
""",
        "sample/CMakeLists.txt",
    )
    reported = {violation.split(": ", 2)[1].split(" ", 1)[0] for violation in graph.violations()}
    return [
        ("an executable linking both runtimes", "direct" in reported, True),
        ("both runtimes through libraries, one of them conditional", "transitive" in reported, True),
        ("an executable linking one runtime", "one_engine" in reported, False),
        ("a library named through a variable is not followed", "chosen" in reported, False),
        ("a runtime chosen by a variable beside a named one", "chosen_and_named" in reported, True),
        ("a runtime chosen through a set() variable beside a named one", "chosen_through_set" in reported, True),
        ("a target made in a foreach over items", "looped_heavy" in reported, True),
        ("a foreach item linking one runtime", "looped" in reported, False),
        ("a target made in a foreach without IN ITEMS", "plain_looped" in reported, True),
        ("a library named in a comment is not linked", "commented" in reported, False),
        ("a generator expression is not followed", "gui" in reported, False),
        (
            "the line of the executable is named",
            any(violation.startswith("sample/CMakeLists.txt:8: direct ") for violation in graph.violations()),
            True,
        ),
    ]


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
        ("Wasmtime's C++ header wasmtime.hh", bool(WASMTIME.c_header.search("#include <wasmtime.hh>")), True),
        ("Wasmtime's C++ header wasm.hh", bool(WASMTIME.c_header.search('#include "wasm.hh"')), True),
        ("a wasmtime/ C++ header", bool(WASMTIME.c_header.search("#include <wasmtime/store.hh>")), True),
        ("WAMR header is not Wasmtime", bool(WASMTIME.c_header.search("#include <wasm_export.h>")), False),
        ("a .hpp of the same stem is not Wasmtime", bool(WASMTIME.c_header.search("#include <wasm.hpp>")), False),
        ("similar name is not Wasmtime", bool(WASMTIME.c_header.search("#include <my_wasm.h>")), False),
        ("Wasmtime backend header", bool(WASMTIME.backend_header.search("#include <vtextension/wasm/wasmtime/WasmtimeEngine.hpp>")), True),
        ("WAMR backend header is not Wasmtime's", bool(WASMTIME.backend_header.search("#include <vtextension/wasm/wamr/WamrBackend.hpp>")), False),
        ("link of contour::wasmtime", bool(WASMTIME.link.search("target_link_libraries(x PRIVATE contour::wasmtime)")), True),
        ("contour::wasmtime is not contour::wamr", bool(WAMR.link.search("target_link_libraries(x contour::wasmtime)")), False),
        ("Wasmtime dir is exempt", is_within(Path("src/vtextension/wasm/wasmtime/WasmtimeModule.cpp"), WASMTIME.directory), True),
        ("WAMR dir is not Wasmtime's", is_within(Path("src/vtextension/wasm/wamr/WamrBackend.cpp"), WASMTIME.directory), False),
    ]
    cases.extend(link_graph_cases())
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
