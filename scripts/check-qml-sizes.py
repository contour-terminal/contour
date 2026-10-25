#! /usr/bin/env python3
"""Keeps numeric size literals out of the chrome QML, against a baseline that only shrinks.

A size in the chrome comes from the design tokens `chromeStyle` publishes (UiStyleProvider), so a restyle
or a scale change reaches it. A bare number in a size property is a size no token can reach. This gate
counts numeric literals other than 0 in the assigned value of the size properties (SIZE_PROPERTIES), in
src/contour/qml/**/*.qml and src/contour/styles/**/*.qml, outside comments. It counts literals, not
lines: `Math.max(a, 8) + 8` is two. `property <type> <name>: N` declarations are not looked at. A value
that wraps onto the next line is counted on its first line only: literals on the continuation lines
are not seen.

scripts/qml-size-literal-baseline.txt holds `path count` lines (repository-relative, `/` separators). A
file must have exactly its baseline count: more fails, fewer also fails, so the baseline only shrinks. A
file not listed has baseline 0, which keeps every new component literal-free. After removing literals,
regenerate it with `python3 scripts/check-qml-sizes.py --print-baseline > scripts/qml-size-literal-baseline.txt`
(keeping its header).

`--self-test` checks the scanner against inline samples and needs no tree. Python rather than shell, so
that ctest runs it on every platform.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# Python puts this script's directory first on sys.path, so the shared helper imports as a sibling.
from qml_lint import code_lines

REPO_ROOT = Path(__file__).resolve().parent.parent
SCAN_ROOTS = (Path("src/contour/qml"), Path("src/contour/styles"))
BASELINE = REPO_ROOT / "scripts" / "qml-size-literal-baseline.txt"

SIZE_PROPERTIES = frozenset(
    {
        "width", "height", "implicitWidth", "implicitHeight", "radius", "spacing",
        "padding", "leftPadding", "rightPadding", "topPadding", "bottomPadding",
        "horizontalPadding", "verticalPadding",
        "leftInset", "rightInset", "topInset", "bottomInset",
        "anchors.margins", "anchors.leftMargin", "anchors.rightMargin",
        "anchors.topMargin", "anchors.bottomMargin",
        "Layout.preferredWidth", "Layout.preferredHeight",
        "Layout.minimumWidth", "Layout.minimumHeight",
        "Layout.maximumWidth", "Layout.maximumHeight",
        "Layout.margins", "Layout.leftMargin", "Layout.rightMargin",
        "Layout.topMargin", "Layout.bottomMargin",
        "font.pixelSize", "font.pointSize", "border.width", "duration",
    }
)  # fmt: skip

# The grouped forms, derived from the dotted names above: the block's name -> the names that are size
# properties inside it.
GROUPED_PROPERTIES = {
    group: frozenset(name.partition(".")[2] for name in SIZE_PROPERTIES if name.startswith(group + "."))
    for group in {name.partition(".")[0] for name in SIZE_PROPERTIES if "." in name}
}

_STATEMENT = re.compile(r"\s*([A-Za-z_][\w.]*)\s*:(?!:)")
_BLOCK_NAME = re.compile(r"([A-Za-z_][\w.]*)\s*$")
_LITERAL = re.compile(r"(?<![\w.])\d+(?:\.\d+)?(?![\w.])")


def _is_size_property(name: str, scope: str) -> bool:
    return name in SIZE_PROPERTIES or name in GROUPED_PROPERTIES.get(scope, frozenset())


def _count_literals(value: str) -> int:
    return sum(1 for token in _LITERAL.findall(value) if float(token) != 0)


def literals(text: str) -> list[tuple[int, int]]:
    """Returns (1-based line number, literal count) for every size assignment holding literals."""
    result: list[tuple[int, int]] = []
    scopes: list[str] = [""]
    for number, line in enumerate(code_lines(text), start=1):
        starts = [(0, scopes[-1])]  # where a statement may begin, with the block it is in
        for index, char in enumerate(line):
            if char == "{":
                found = _BLOCK_NAME.search(line[:index])
                scopes.append(found.group(1) if found else "")
                starts.append((index + 1, scopes[-1]))
            elif char == "}":
                if len(scopes) > 1:
                    scopes.pop()
            elif char == ";":
                starts.append((index + 1, scopes[-1]))
        for begin, scope in starts:
            found = _STATEMENT.match(line, begin)
            if not found or not _is_size_property(found.group(1), scope):
                continue
            value = re.split(r"[;{}]", line[found.end() :], maxsplit=1)[0]
            if count := _count_literals(value):
                result.append((number, count))
    return result


def counts_by_file(files: dict[str, str]) -> dict[str, int]:
    """Returns the literal count of each file that has any, keyed by its repository-relative path."""
    counts = {path: sum(count for _, count in literals(text)) for path, text in files.items()}
    return {path: count for path, count in counts.items() if count}


def load_baseline(text: str) -> dict[str, int]:
    baseline: dict[str, int] = {}
    for raw in text.splitlines():
        line = raw.strip()
        if line and not line.startswith("#"):
            path, _, count = line.rpartition(" ")
            baseline[path] = int(count)
    return baseline


def compare(counts: dict[str, int], baseline: dict[str, int]) -> list[str]:
    """Returns one message per file whose count differs from its baseline (unlisted: 0)."""
    messages: list[str] = []
    for path in sorted(counts.keys() | baseline.keys()):
        have, allowed = counts.get(path, 0), baseline.get(path, 0)
        if have > allowed:
            messages.append(f"{path}: {have - allowed} new size literal(s) ({have}, baseline {allowed})")
        elif have < allowed:
            messages.append(f"{path}: lower the baseline of {path} to {have} (was {allowed})")
    return messages


def read_tree() -> dict[str, str]:
    return {
        path.relative_to(REPO_ROOT).as_posix(): path.read_text(encoding="utf-8")
        for root in SCAN_ROOTS
        for path in sorted((REPO_ROOT / root).rglob("*.qml"))
    }


def format_baseline(counts: dict[str, int]) -> str:
    return "".join(f"{path} {count}\n" for path, count in sorted(counts.items()))


def check_tree() -> int:
    baseline = load_baseline(BASELINE.read_text(encoding="utf-8")) if BASELINE.exists() else {}
    messages = compare(counts_by_file(read_tree()), baseline)
    for message in messages:
        print(message)
    if messages:
        print(
            f"check-qml-sizes: {len(messages)} file(s) off their baseline. Take sizes from chromeStyle "
            "(UiStyleProvider); the baseline only shrinks, see scripts/check-qml-sizes.py.",
            file=sys.stderr,
        )
        return 1
    print("check-qml-sizes: every file is at its baseline.")
    return 0


def self_test() -> int:
    def total(source: str) -> int:
        return sum(count for _, count in literals(source))

    cases: list[tuple[str, int, str]] = [
        ("width: 120", 1, "width literal"),
        ("anchors.leftMargin: 8", 1, "dotted anchors margin"),
        ("implicitHeight: Math.max(a, b) + 8", 1, "literal after a call"),
        ("implicitHeight: label.implicitHeight + 2 * chromeStyle.space.s3", 1, "multiplier on a token"),
        ("font.pointSize: 14", 1, "font point size"),
        ("duration: 160", 1, "duration"),
        ("radius: 4.5", 1, "fractional literal"),
        ("Layout.maximumWidth: 360", 1, "Layout property"),
        ("foo { width: 120; height: 4 }", 2, "two assignments on one line"),
        ("implicitWidth: Math.max(a, 8) + 8", 2, "literals, not lines"),
        ("anchors { margins: 8 }", 1, "grouped anchors"),
        ("font { pixelSize: 12 }", 1, "grouped font"),
        ("Layout { preferredWidth: 90 }", 1, "grouped Layout"),
        ("border { width: 2 }", 1, "grouped border"),
        ("anchors {\n  leftMargin: 8\n}", 1, "grouped, multi-line"),
        ("margins: 8", 0, "margins outside a group"),
        ("Rectangle { margins: 8 }", 0, "margins in another block"),
        ("anchors { margins: 8 }\nmargins: 8", 1, "group scope ends with its block"),
        ("height: visible ? implicitHeight : 0", 0, "only zero"),
        ("width: chromeStyle.size.toast", 0, "token"),
        ("anchors.margins: chromeStyle.space.s6", 0, "token with digits in its name"),
        ("opacity: 0.5", 0, "not a size property"),
        ("z: 11", 0, "not a size property"),
        ("// width: 120", 0, "comment"),
        ('text: "width: 120"', 0, "string"),
        ("property real gap: 8", 0, "property declaration is out of scope"),
    ]
    failures = [
        f"{description}: expected {expected}, got {total(source)}"
        for source, expected, description in cases
        if total(source) != expected
    ]

    baseline = load_baseline("# header\nsrc/contour/qml/a.qml 2\n\nsrc/contour/qml/b.qml 1\n")
    if compare({"src/contour/qml/a.qml": 2, "src/contour/qml/b.qml": 1}, baseline):
        failures.append("a file at its count passes")
    if not any("1 new size literal(s)" in m for m in compare({"src/contour/qml/a.qml": 3, "src/contour/qml/b.qml": 1}, baseline)):
        failures.append("a file above its count fails")
    if not any("lower the baseline of src/contour/qml/a.qml to 1" in m for m in compare({"src/contour/qml/a.qml": 1, "src/contour/qml/b.qml": 1}, baseline)):
        failures.append("a file below its count fails with the lower-it message")
    if not any("c.qml" in m and "new size literal(s)" in m for m in compare({"src/contour/qml/a.qml": 2, "src/contour/qml/b.qml": 1, "src/contour/qml/c.qml": 1}, baseline)):
        failures.append("an unlisted file with a literal fails")
    printed = format_baseline(counts_by_file({"src/contour/qml/a.qml": "width: 1", "src/contour/qml/z.qml": "x: 1"}))
    if printed != "src/contour/qml/a.qml 1\n":
        failures.append("--print-baseline writes posix paths and skips clean files")

    for description in failures:
        print(f"check-qml-sizes self-test: FAILED: {description}", file=sys.stderr)
    if failures:
        print(f"check-qml-sizes self-test: {len(failures)} case(s) failed.", file=sys.stderr)
        return 1
    print(f"check-qml-sizes self-test: {len(cases) + 5} cases OK.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--self-test", action="store_true", help="check the scanner against inline samples")
    parser.add_argument("--print-baseline", action="store_true", help="print the current counts as baseline lines")
    arguments = parser.parse_args()
    if arguments.self_test:
        return self_test()
    if arguments.print_baseline:
        print(format_baseline(counts_by_file(read_tree())), end="")
        return 0
    return check_tree()


if __name__ == "__main__":
    sys.exit(main())
