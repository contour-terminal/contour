#! /usr/bin/env python3
"""Keeps the keyboard pipeline free of platform tests below the normalizer.

A key press is normalized once, per platform, into a KeyEvent (src/contour/input/QtKeyCapture.cpp and
the three KeyboardPlatform implementations choose by data, not by macro). Everything beneath that seam
-- the router, the default-binding table, the session's key path -- is platform-neutral and tested on
every host. This gate fails when a platform test creeps back into one of those files:

* a preprocessor condition (#if, #ifdef, #ifndef, #elif, #elifdef, #elifndef, defined(...)) naming
  __APPLE__, _WIN32, _WIN64, __linux__, __unix__ or Q_OS_*;
* the identifiers BuildKeyboardPlatformKind or BuildPaneChordHost in code: the build host's kind is read
  once at the composition root and handed down, never consulted below it.

The allowed homes (input/QtKeyCapture.cpp, input/KeyboardLayout.cpp, input/XkbLiveKeymap.cpp,
config/KeyboardPlatformKind.hpp and CMake) are simply not in the list below. Every offending line is
printed as file:line. `--self-test` checks the patterns against inline fixtures and needs no tree.
Python rather than shell, so that ctest runs it on every platform.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
CONTOUR_ROOT = Path("src/contour")

# Files (or globs, relative to src/contour) that must stay platform-neutral.
CHECKED_GLOBS = (
    "input/KeyEvent.hpp",
    "input/NativeKeyEvent.*",
    "input/KeyboardPlatform*.hpp",
    "input/KeyboardPlatform*.cpp",
    "input/MacKeyboardPlatform.cpp",
    "input/Win32KeyboardPlatform.cpp",
    "input/XkbKeyboardPlatform.cpp",
    "input/KeyRouter.*",
    "input/KeyTranslation.hpp",
    "input/XkbKeymap.hpp",
    "input/KeysymTable.hpp",
    "session/SessionInput.cpp",
    "session/TerminalSessionKeyInput.cpp",
    "config/DefaultBindings.*",
)

CONDITIONAL = re.compile(r"^\s*#\s*(?:if|ifdef|ifndef|elif|elifdef|elifndef)\b(?P<condition>.*)$")
PLATFORM_MACRO = re.compile(r"\b(?:__APPLE__|_WIN32|_WIN64|__linux__|__unix__|Q_OS_\w+)\b")
HOST_IDENTIFIER = re.compile(r"\b(?:BuildKeyboardPlatformKind|BuildPaneChordHost)\b")
LINE_COMMENT = re.compile(r"//.*$")

MESSAGE = "platform test below the normalizer"


def violates(line: str) -> bool:
    """Whether the source `line` is a platform test (comments are ignored)."""
    code = LINE_COMMENT.sub("", line)
    conditional = CONDITIONAL.match(code)
    if conditional and PLATFORM_MACRO.search(conditional.group("condition")):
        return True
    return bool(HOST_IDENTIFIER.search(code))


def checked_files() -> list[Path]:
    """The existing files the gate covers, sorted, repository-relative."""
    found: set[Path] = set()
    for pattern in CHECKED_GLOBS:
        found.update(path for path in (REPO_ROOT / CONTOUR_ROOT).glob(pattern) if path.is_file())
    return sorted(found)


def scan() -> list[str]:
    """Every violation, as `file:line: message`."""
    violations: list[str] = []
    for absolute in checked_files():
        shown = absolute.relative_to(REPO_ROOT).as_posix()
        text = absolute.read_text(encoding="utf-8", errors="replace")
        for number, line in enumerate(text.splitlines(), start=1):
            if violates(line):
                violations.append(f"{shown}:{number}: {MESSAGE}")
    return violations


def self_test() -> int:
    """Checks the patterns against inline fixtures: one clean, one per pattern."""
    cases: list[tuple[str, str, bool]] = [
        # (description, source line, expected to violate)
        ("clean code", "auto const kind = platform.kind();", False),
        ("clean include", "#include <contour/input/KeyEvent.hpp>", False),
        ("unrelated #if", "#if QT_CONFIG(xcb)", False),
        ("platform named in a comment", "// not an #ifdef __APPLE__ any more", False),
        ("platform named in a string", 'auto name = "__APPLE__";', False),
        ("#ifdef __APPLE__", "#ifdef __APPLE__", True),
        ("#ifndef _WIN32", "#ifndef _WIN32", True),
        ("#if _WIN64", "#if _WIN64", True),
        ("indented # if __linux__", "  #  if defined(__linux__)", True),
        ("#elif __unix__", "#elif __unix__", True),
        ("#elifdef _WIN32", "#elifdef _WIN32", True),
        ("#if defined(Q_OS_MACOS)", "#if defined(Q_OS_MACOS)", True),
        ("#if with several macros", "#if !defined(__APPLE__) && !defined(_WIN32)", True),
        ("BuildKeyboardPlatformKind use", "auto k = config::BuildKeyboardPlatformKind;", True),
        ("BuildPaneChordHost use", "return BuildPaneChordHost;", True),
        ("longer identifier is not the host constant", "auto k = BuildKeyboardPlatformKindCount;", False),
    ]
    failures = [description for description, line, expected in cases if violates(line) != expected]
    for description in failures:
        print(f"check-input-platform-neutral self-test: FAILED: {description}", file=sys.stderr)
    if failures:
        print(
            f"check-input-platform-neutral self-test: {len(failures)} of {len(cases)} cases failed.",
            file=sys.stderr,
        )
        return 1
    print(f"check-input-platform-neutral self-test: {len(cases)} cases OK.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true", help="check the patterns against inline fixtures")
    arguments = parser.parse_args()
    if arguments.self_test:
        return self_test()

    files = checked_files()
    if not files:
        print("check-input-platform-neutral: no file in the list exists; the gate would pass vacuously.", file=sys.stderr)
        return 1
    violations = scan()
    for violation in violations:
        print(violation, file=sys.stderr)
    if violations:
        print(f"check-input-platform-neutral: {len(violations)} violation(s).", file=sys.stderr)
        return 1
    print(f"check-input-platform-neutral: {len(files)} files are platform-neutral.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
