#! /usr/bin/env python3
"""Keeps colour literals out of the chrome QML: a colour comes from the style or from the palette.

The chrome takes its colours from two places only: the OS palette (SystemPalette, `palette`), and the
design tokens `chromeStyle` publishes (UiStyleProvider). A hex literal in a .qml file is a third,
invisible source that no theme, no dark/light switch and no restyle can reach. This gate fails on, in
src/contour/qml/**/*.qml and src/contour/styles/**/*.qml, outside comments:

1. A hex colour literal: "#RRGGBB" or "#AARRGGBB" (in either quote style).
2. A Qt.rgba( call whose four arguments are all numeric literals. A derivation such as
   Qt.rgba(c.r, c.g, c.b, 0.5) from a palette or token colour is fine: it follows its source.

Lines listed in scripts/qml-color-allowlist.txt are exempt. Each entry is `path:fragment`, where path is
repository-relative with forward slashes and fragment is text that must occur on the offending line;
blank lines and lines starting with `#` are ignored. The list is for colours that are protocol-defined
data, never for a colour that merely is not worth moving.

Every offending line is printed as path:line. `--self-test` checks the scanner against inline samples
and needs no tree. Python rather than shell, so that ctest runs it on every platform.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from qml_lint import code_lines  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent
SCAN_ROOTS = (Path("src/contour/qml"), Path("src/contour/styles"))
ALLOWLIST = REPO_ROOT / "scripts" / "qml-color-allowlist.txt"

HEX_LITERAL = re.compile(r"""["']#(?:[0-9a-fA-F]{8}|[0-9a-fA-F]{6})["']""")
_NUMBER = r"[+-]?(?:\d+\.?\d*|\.\d+)"
RGBA_LITERAL = re.compile(r"\bQt\.rgba\(\s*" + r"\s*,\s*".join([_NUMBER] * 4) + r"\s*\)")


def offences(text: str) -> list[tuple[int, str]]:
    """Returns (1-based line number, original line) for every colour literal in code."""
    originals = text.splitlines()
    return [
        (number, originals[number - 1])
        for number, code in enumerate(code_lines(text), start=1)
        if HEX_LITERAL.search(code) or RGBA_LITERAL.search(code)
    ]


def load_allowlist(path: Path) -> list[tuple[str, str]]:
    entries: list[tuple[str, str]] = []
    if not path.exists():
        return entries
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        file, _, fragment = line.partition(":")
        entries.append((file.strip(), fragment.strip()))
    return entries


def is_allowed(relative: str, line: str, allowlist: list[tuple[str, str]]) -> bool:
    return any(file == relative and fragment and fragment in line for file, fragment in allowlist)


def check_tree() -> int:
    allowlist = load_allowlist(ALLOWLIST)
    failures = 0
    for root in SCAN_ROOTS:
        for path in sorted((REPO_ROOT / root).rglob("*.qml")):
            relative = path.relative_to(REPO_ROOT).as_posix()
            text = path.read_text(encoding="utf-8")
            for number, line in offences(text):
                if is_allowed(relative, line, allowlist):
                    continue
                failures += 1
                print(f"{relative}:{number}: colour literal: {line.strip()}")
    if failures:
        print(
            f"check-qml-colors: {failures} colour literal(s). Take the colour from the palette or from "
            "chromeStyle (UiStyleProvider); see scripts/check-qml-colors.py.",
            file=sys.stderr,
        )
        return 1
    print("check-qml-colors: no colour literals.")
    return 0


def self_test() -> int:
    allowlist = [("a.qml", 'color: "#00000000"')]
    cases: list[tuple[str, bool, str]] = [
        ('color: "#c62828"', True, "6-digit hex in code"),
        ("color: '#c62828'", True, "6-digit hex, single quotes"),
        ('color: "#99000000"', True, "8-digit hex in code"),
        ("color: Qt.rgba(0, 0, 0, 0.6)", True, "all-numeric Qt.rgba"),
        ("color: Qt.rgba(0.9,0.2,0.2,.16)", True, "all-numeric Qt.rgba, tight spacing"),
        ("color: Qt.rgba(c.r, c.g, c.b, 0.5)", False, "derived Qt.rgba from a colour"),
        ("color: Qt.rgba(0, 0, 0, alpha)", False, "Qt.rgba with a non-literal alpha"),
        ('color: "white"', False, "named colour is not a hex literal"),
        ('color: "#fff"', False, "3-digit hex is outside the rule"),
        ('text: "#" + field.text', False, "hash string concatenation"),
        ('// color: "#c62828"', False, "hex in a // comment"),
        ('x: 1 // was "#c62828"', False, "hex in a trailing // comment"),
        ("// Qt.rgba(0, 0, 0, 1)", False, "Qt.rgba in a // comment"),
        ('/* color: "#c62828" */', False, "hex in a one-line block comment"),
        ('/* a\n color: "#c62828"\n Qt.rgba(1, 1, 1, 1) */', False, "literals in a multi-line block comment"),
        ('/* x */ color: "#c62828"', True, "code after a closed block comment"),
        ('url: "http://x/#c62828"', False, "// inside a string is not a comment, and no whole-string hex"),
        ('a: "http://x"; b: "#c62828"', True, "// inside a string does not hide a later literal"),
    ]
    failures = [
        description
        for source, expected, description in cases
        if bool(offences(source)) != expected
    ]
    allowed_line = 'color: "#00000000"'
    if is_allowed("a.qml", allowed_line, allowlist) is not True:
        failures.append("allowlisted line passes")
    if is_allowed("b.qml", allowed_line, allowlist):
        failures.append("allowlist entry is scoped to its path")
    if is_allowed("a.qml", 'color: "#c62828"', allowlist):
        failures.append("allowlist entry is scoped to its fragment")
    for description in failures:
        print(f"check-qml-colors self-test: FAILED: {description}", file=sys.stderr)
    if failures:
        print(f"check-qml-colors self-test: {len(failures)} case(s) failed.", file=sys.stderr)
        return 1
    print(f"check-qml-colors self-test: {len(cases) + 3} cases OK.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--self-test", action="store_true", help="check the scanner against inline samples")
    arguments = parser.parse_args()
    return self_test() if arguments.self_test else check_tree()


if __name__ == "__main__":
    sys.exit(main())
