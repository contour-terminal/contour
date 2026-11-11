#! /usr/bin/env python3
"""Keeps fork() and forkpty() out of first-party code that runs in a threaded process.

Contour is multithreaded: a fork() made while another thread holds a libc lock that an at-fork handler
also takes can deadlock the child or the parent (#2181 was exactly that, in setlocale() on macOS).
Every first-party spawn is therefore a posix_spawn of the running executable in trampoline mode
(src/vtpty/SpawnTrampoline_unix.cpp); only that single-threaded trampoline may fork. This gate fails on
a call to fork(, vfork( or forkpty( in src/**/*.{cpp,hpp}, ignoring comments and string literals, and
prints every offender as file:line. Test code (*_test.cpp and any src/*/test/ directory) is exempt, and
so are the files in ALLOWED below, each with its reason. `--self-test` checks the scanner against inline
fixtures and needs no tree. Python rather than shell, so that ctest runs it on every platform.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SOURCE_ROOT = Path("src")

# Files that may fork, repository-relative, with the reason.
ALLOWED: dict[str, str] = {
    "src/vtpty/SpawnTrampoline_unix.cpp": "single-threaded trampoline",
    "src/crispy/StackTrace.cpp": "crash path, out of scope",
    "src/vthost/keeper/PosixPorts.cpp": (
        "the PTY keeper: its child is async-signal-safe up to the exec; PR_SET_PDEATHSIG and the "
        "pthread_sigmask/signalfd setup are what need the keeper single-threaded"
    ),
}

FORK_CALL = re.compile(r"\b(?:v?fork|forkpty)\s*\(")
STRING_LITERAL = re.compile(r'"(?:\\.|[^"\\])*"')
CHAR_LITERAL = re.compile(r"'(?:\\.|[^'\\])+'")

MESSAGE = "fork from a threaded process; spawn through vtpty::SpawnTrampoline instead"


def code_lines(text: str) -> list[tuple[int, str]]:
    """The (line number, line) pairs of `text` with comments and literals blanked out.

    Whichever of `//` and `/*` comes first on a line wins, so a `/*` inside a line comment (a glob such as
    `profiles/*.yml`) opens nothing. A call split across lines (`fork` newline `(`) is not seen: a known
    limitation, the project formats such a call onto one line.
    """
    result: list[tuple[int, str]] = []
    in_block = False
    for number, raw in enumerate(text.splitlines(), start=1):
        line = CHAR_LITERAL.sub("''", STRING_LITERAL.sub('""', raw))
        kept = ""
        while line:
            if in_block:
                end = line.find("*/")
                if end < 0:
                    line = ""
                else:
                    line = line[end + 2 :]
                    in_block = False
                continue
            line_start = line.find("//")
            block_start = line.find("/*")
            if line_start >= 0 and (block_start < 0 or line_start < block_start):
                kept += line[:line_start]
                line = ""
            elif block_start >= 0:
                kept += line[:block_start] + " "
                line = line[block_start + 2 :]
                in_block = True
            else:
                kept += line
                line = ""
        result.append((number, kept))
    return result


def violating_lines(text: str) -> list[int]:
    """The numbers of the lines of `text` that call fork, vfork or forkpty."""
    return [number for number, line in code_lines(text) if FORK_CALL.search(line)]


def is_exempt(relative: Path) -> bool:
    """Whether the repository-relative `relative` is test code or allowlisted."""
    if relative.name.endswith("_test.cpp"):
        return True
    if len(relative.parts) > 2 and relative.parts[2] == "test":
        return True
    return relative.as_posix() in ALLOWED


def checked_files() -> list[Path]:
    """The source files the gate covers, sorted, repository-relative."""
    found = [
        path.relative_to(REPO_ROOT)
        for pattern in ("*.cpp", "*.hpp")
        for path in (REPO_ROOT / SOURCE_ROOT).rglob(pattern)
        if path.is_file()
    ]
    return sorted(path for path in found if not is_exempt(path))


def scan() -> list[str]:
    """Every violation, as `file:line: message`."""
    violations: list[str] = []
    for relative in checked_files():
        text = (REPO_ROOT / relative).read_text(encoding="utf-8", errors="replace")
        violations.extend(f"{relative.as_posix()}:{number}: {MESSAGE}" for number in violating_lines(text))
    return violations


def self_test() -> int:
    """Checks the scanner against inline fixtures: clean ones and one per pattern."""
    cases: list[tuple[str, str, bool]] = [
        # (description, source, expected to violate)
        ("global fork", "auto p = ::fork();", True),
        ("forkpty", "auto pid = forkpty(&master, nullptr, nullptr, nullptr);", True),
        ("vfork", "auto p = vfork ();", True),
        ("fork in a comment", "// fork() is gone", False),
        ("fork in a doc comment", "/// between fork() and exec()", False),
        ("fork in a block comment", "/* fork() */ int a;", False),
        ("fork in a multi-line block comment", "/*\n * fork()\n */\nint a;", False),
        ("code after a block comment", "/* x */ auto p = fork();", True),
        ("block opener inside a line comment", "// see profiles/*.yml\nauto p = fork();", True),
        ("block opener inside a line comment, clean", "// see profiles/*.yml\nauto p = 1;", False),
        ("fork in a string", 'auto s = "fork()";', False),
        ("longer identifier", "auto p = forkChild();", False),
        ("longer identifier, prefixed", "auto p = spoon_fork();", False),
        ("member named fork", "auto p = tree.forks;", False),
    ]
    failures = [description for description, source, expected in cases if bool(violating_lines(source)) != expected]
    exempt = [
        ("test file", "src/vtpty/Process_test.cpp", True),
        ("test directory", "src/vtpty/test/GateTesting.hpp", True),
        ("allowlisted", "src/crispy/StackTrace.cpp", True),
        ("ordinary source", "src/vtpty/Process_unix.cpp", False),
    ]
    failures += [description for description, path, expected in exempt if is_exempt(Path(path)) != expected]
    for description in failures:
        print(f"check-no-threaded-fork self-test: FAILED: {description}", file=sys.stderr)
    total = len(cases) + len(exempt)
    if failures:
        print(f"check-no-threaded-fork self-test: {len(failures)} of {total} cases failed.", file=sys.stderr)
        return 1
    print(f"check-no-threaded-fork self-test: {total} cases OK.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true", help="check the scanner against inline fixtures")
    arguments = parser.parse_args()
    if arguments.self_test:
        return self_test()

    stale = [path for path in ALLOWED if not (REPO_ROOT / path).is_file()]
    for path in stale:
        print(f"check-no-threaded-fork: allowlisted file {path} does not exist; remove its entry.", file=sys.stderr)
    files = checked_files()
    if not files:
        print("check-no-threaded-fork: no source file found; the gate would pass vacuously.", file=sys.stderr)
        return 1
    violations = scan()
    for violation in violations:
        print(violation, file=sys.stderr)
    if violations or stale:
        print(f"check-no-threaded-fork: {len(violations)} violation(s).", file=sys.stderr)
        return 1
    print(f"check-no-threaded-fork: {len(files)} files are free of fork().")
    return 0


if __name__ == "__main__":
    sys.exit(main())
