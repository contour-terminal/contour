#! /usr/bin/env python3
"""Keeps the Windows SDK's lowercase object-like macros out of first-party identifiers.

<windows.h> defines `near` and `far` as empty macros (minwindef.h), `pascal` and `cdecl` as calling
conventions, and rpcndr.h defines `small` as `char` and `hyper` as `__int64`. A first-party identifier of
one of these names is rewritten by the preprocessor in any translation unit that sees the SDK, directly or
through a header, and the build breaks only on Windows: `std::optional<std::string> near;` becomes a
member with no name. MSVC, a C++20 compiler with modules always on, also reads a line that starts with
`module` or `import` followed by `;`, `:`, `<` or a name as a module directive: a member `module` that
clang-format wraps onto a line of its own (`std::filesystem::path\n    module;`) becomes a global module
fragment. This gate fails on either in src/ and extensions/, ignoring comments and string literals, and
prints every offender as file:line. Test code is not exempt: it builds on Windows too.
Generated wit-bindgen bindings (extensions/*/guest/bindings/) are C for the wasm guest only and are
skipped. `--self-test` checks the scanner against inline fixtures and needs no tree. Python rather than
shell, so that ctest runs it on every platform.
"""

from __future__ import annotations

import argparse
import re
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
# The first-party trees: Contour itself, and the extensions beside it.
SOURCE_ROOTS = (Path("src"), Path("extensions"))
SOURCE_SUFFIXES = (".cpp", ".hpp", ".h", ".c", ".cc", ".mm", ".ixx", ".cppm")

# The Windows SDK's lowercase object-like macros, with what each expands to. Function-like ones
# (min, max) are not here: they do not touch an identifier that is not followed by `(`.
MACROS: dict[str, str] = {
    "near": "nothing (minwindef.h)",
    "far": "nothing (minwindef.h)",
    "pascal": "__stdcall (minwindef.h)",
    "cdecl": "__cdecl (windef.h)",
    "small": "char (rpcndr.h)",
    "hyper": "__int64 (rpcndr.h)",
}

MACRO_NAME = re.compile(r"\b(" + "|".join(MACROS) + r")\b")
# A line MSVC reads as a module or import directive ([cpp.pre]): `module` or `import`, optionally after
# `export`, first on the line and followed by `;`, `:`, `<` or an identifier.
MODULE_DIRECTIVE = re.compile(r"^\s*(?:export\s+)?(module|import)\b(?=\s*[;:<A-Za-z_])")
# What is not code, in the order a scanner meets it: a raw string literal (any delimiter, any number of
# lines), a block comment, a line comment, a string literal, a character literal. A digit separator
# (1'000) is not a character literal: the quote is preceded by an alphanumeric character.
NOT_CODE = re.compile(
    r'''(?:\b(?:u8|[uUL])?R"(?P<delimiter>[^()\\\s]{0,16})\((?s:.*?)\)(?P=delimiter)")'''
    r"|/\*(?s:.*?)\*/"
    r"|//[^\n]*"
    r'''|"(?:\\.|[^"\\\n])*"'''
    r"|(?<![0-9A-Za-z_])'(?:\\.|[^'\\\n])+'"
)


def code_lines(text: str) -> list[tuple[int, str]]:
    """The (line number, line) pairs of `text` with comments and literals blanked out.

    Each comment or literal becomes spaces, its line breaks kept, so line numbers still match the file. A `/*`
    inside a line comment (a glob such as `profiles/*.yml`) opens nothing, as the leftmost match wins.
    """
    blanked = NOT_CODE.sub(lambda match: re.sub(r"[^\n]", " ", match.group(0)), text)
    return list(enumerate(blanked.split("\n"), start=1))


def violations_in(text: str) -> list[tuple[int, str]]:
    """The (line number, name) of every identifier in `text` that a Windows SDK macro rewrites, and of every
    `module` or `import` that starts a line MSVC reads as a module directive."""
    found: list[tuple[int, str]] = []
    for number, line in code_lines(text):
        found.extend((number, match.group(1)) for match in MACRO_NAME.finditer(line))
        found.extend((number, match.group(1)) for match in MODULE_DIRECTIVE.finditer(line))
    return found


def message_for(name: str) -> str:
    """What is wrong with the identifier `name` that violations_in reported."""
    if name in MACROS:
        return f"`{name}` is a Windows SDK macro expanding to {MACROS[name]}; rename the identifier"
    return f"`{name}` starts a line MSVC reads as a module directive; rename the identifier"


def is_skipped(relative: Path) -> bool:
    """Whether the repository-relative `relative` is generated guest bindings."""
    parts = relative.parts
    return len(parts) > 3 and parts[0] == "extensions" and parts[2:4] == ("guest", "bindings")


def checked_files(repo_root: Path = REPO_ROOT) -> list[Path]:
    """The source files the gate covers in the tree at `repo_root`, sorted, repository-relative."""
    found = [
        path.relative_to(repo_root)
        for root in SOURCE_ROOTS
        for path in (repo_root / root).rglob("*")
        if path.suffix in SOURCE_SUFFIXES and path.is_file()
    ]
    return sorted(path for path in found if not is_skipped(path))


def scan(repo_root: Path = REPO_ROOT) -> list[str]:
    """Every violation in the tree at `repo_root`, as `file:line: message`."""
    violations: list[str] = []
    for relative in checked_files(repo_root):
        text = (repo_root / relative).read_text(encoding="utf-8", errors="replace")
        violations.extend(f"{relative.as_posix()}:{number}: {message_for(name)}" for number, name in violations_in(text))
    return violations


def self_test() -> int:
    """Checks the scanner against inline fixtures: clean ones and one per macro."""
    cases: list[tuple[str, str, bool]] = [
        # (description, source, expected to violate)
        *((f"member named {name}", f"std::optional<std::string> {name};", True) for name in MACROS),
        ("parameter", "void open(std::optional<std::string_view> near);", True),
        ("designated initializer", "auto s = Spawn { .near = tab };", True),
        ("member access", "if (spawn.far) {}", True),
        ("local", "auto const near = row(1);", True),
        ("in a line comment", "int a; // opens near its dashboard", False),
        ("in a doc comment", "/// How far reading it has come.", False),
        ("in a block comment", "/* near */ int a;", False),
        ("in a multi-line block comment", "/*\n * far\n */\nint a;", False),
        ("code after a block comment", "/* x */ int far;", True),
        ("block opener inside a line comment", "// see profiles/*.yml\nint near;", True),
        ("in a string", 'auto s = "near: option<string>";', False),
        ("in a raw string", 'CHECK(json.contains(R"("near":"tab")"));', False),
        ("in a delimited multi-line raw string", 'auto s = R"x(\n"near")\nsmall\n)x";\nint a;', False),
        ("after a raw string", 'auto s = R"(far)"; int near;', True),
        ("escaped quote in a string", 'auto s = "a\\"near"; int a;', False),
        ("after a digit separator", "auto n = 1'000; int far;", True),
        ("in a character literal", "auto c = '\\''; int a;", False),
        ("longer identifier", "std::optional<std::string> nearTab;", False),
        ("prefixed identifier", "auto smallest = 1; auto farAway = 2; auto hyperlink = 3;", False),
        ("snake case", "int is_near = 0;", False),
        ("member wrapped onto its own line", "std::filesystem::path\n    module; ///< The module.", True),
        ("module followed by a name", "module foo;", True),
        ("module partition", "module :private;", True),
        ("exported module", "export module foo;", True),
        ("import of a name", "    import std;", True),
        ("import of a header", "import <vector>;", True),
        ("module member access", "auto const path = declaration.module;", False),
        ("module mid-line", "auto const module = load(bytes);", False),
        ("module first, then a dot", "module.size();", False),
        ("module first, then an assignment", "module = {};", False),
        ("import first, then a call", "import(row);", False),
        ("module in a comment", "/// module: the module on disk", False),
        ("module prefixing a name", "moduleDigest = 0;", False),
    ]
    failures = [description for description, source, expected in cases if bool(violations_in(source)) != expected]
    skipped = [
        ("generated bindings", "extensions/orchestrator/guest/bindings/daemon.c", True),
        ("guest driver", "extensions/orchestrator/guest/Driver.cpp", False),
        ("test file", "src/vtbackend/shell/Folding_test.cpp", False),
        ("host source", "src/vthost/ExtensionHosting.hpp", False),
    ]
    failures += [description for description, path, expected in skipped if is_skipped(Path(path)) != expected]
    # Every scan root and suffix is walked: a fixture tree with one offender in each must report all of them.
    roots = [
        ("src .hpp is scanned", "src/a/Spawn.hpp"),
        ("src .c is scanned", "src/a/spawn.c"),
        ("src .mm is scanned", "src/a/Spawn.mm"),
        ("extensions are scanned", "extensions/other/core/Spawn.cpp"),
    ]
    with tempfile.TemporaryDirectory() as tmp:
        for _, path in roots:
            (Path(tmp) / path).parent.mkdir(parents=True, exist_ok=True)
            (Path(tmp) / path).write_text("int near;\n")
        reported = scan(Path(tmp))
    failures += [description for description, path in roots if not any(v.startswith(f"{path}:1:") for v in reported)]
    for description in failures:
        print(f"check-windows-macro-identifiers self-test: FAILED: {description}", file=sys.stderr)
    total = len(cases) + len(skipped) + len(roots)
    if failures:
        print(f"check-windows-macro-identifiers self-test: {len(failures)} of {total} cases failed.", file=sys.stderr)
        return 1
    print(f"check-windows-macro-identifiers self-test: {total} cases OK.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true", help="check the scanner against inline fixtures")
    arguments = parser.parse_args()
    if arguments.self_test:
        return self_test()

    files = checked_files()
    if not files:
        print("check-windows-macro-identifiers: no source file found; the gate would pass vacuously.", file=sys.stderr)
        return 1
    violations = scan()
    for violation in violations:
        print(violation, file=sys.stderr)
    if violations:
        print(f"check-windows-macro-identifiers: {len(violations)} violation(s).", file=sys.stderr)
        return 1
    print(f"check-windows-macro-identifiers: {len(files)} files are free of Windows SDK macro names and module lines.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
