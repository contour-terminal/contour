#! /usr/bin/env python3
"""Keeps the headers that were made light from growing heavy again.

    check-include-budget.py [--root DIR] [--budget FILE]
    check-include-budget.py --self-test

scripts/include-budget.txt holds rows `<header> !<forbidden>`: <header> must not transitively include
<forbidden>. Both are repository-relative paths (src/contour/...); <forbidden> may instead be an
angle-bracket spelling such as `!<QtDBus/QDBusVariant>`, matched against the spelling of the #include
lines (for headers that are not first-party). A row fails when the forbidden header is reachable, and
the failure names the include chain (a.hpp -> b.hpp -> c.hpp). A row naming a header that does not
exist also fails ("no such header"), so a rename cannot turn a row into a vacuous pass.

The walk follows first-party `#include` lines, quoted or angle-bracket, resolved against the include
roots (src/ and vendor/core-cpp/src/) and, for quoted ones, the including file's directory. It ignores
`#if`: every include in a header counts whichever branch it sits in. That is deliberate, a conservative
upper bound -- a header that reaches the forbidden one on any platform fails everywhere. Includes that
resolve to no file (system and Qt headers) are leaves, known by spelling only.

`--self-test` checks the walker on a temporary tree. Python rather than shell, so ctest runs it on
every platform.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
import tempfile
from collections import deque
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
BUDGET = REPO_ROOT / "scripts" / "include-budget.txt"
INCLUDE_ROOTS = (Path("src"), Path("vendor/core-cpp/src"))

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]', re.MULTILINE)
ROW_RE = re.compile(r"^(\S+)\s+!(\S+)$")


def normalize(path: Path) -> Path:
    """Collapses `..` and `.` lexically, so <contour/../contour/X.hpp> names one file."""
    return Path(os.path.normpath(path))


class IncludeGraph:
    """First-party include edges, read lazily and cached."""

    def __init__(self, root: Path) -> None:
        self.root = root
        self._edges: dict[Path, list[tuple[str, Path | None]]] = {}

    def resolve(self, includer: Path, bracket: str, spelling: str) -> Path | None:
        candidates = []
        if bracket == '"':
            candidates.append(includer.parent / spelling)
        candidates.extend(root / spelling for root in INCLUDE_ROOTS)
        for candidate in candidates:
            candidate = normalize(candidate)
            if (self.root / candidate).is_file():
                return candidate
        return None

    def edges(self, header: Path) -> list[tuple[str, Path | None]]:
        """(spelling, resolved path or None) for each #include in `header`."""
        if header not in self._edges:
            text = (self.root / header).read_text(encoding="utf-8", errors="replace")
            self._edges[header] = [
                (spelling, self.resolve(header, bracket, spelling))
                for bracket, spelling in INCLUDE_RE.findall(text)
            ]
        return self._edges[header]

    def find_chain(self, start: Path, forbidden: str) -> list[str] | None:
        """Shortest include chain from `start` to `forbidden` (a path, or a `<spelling>`), if any."""
        want_spelling = forbidden[1:-1] if forbidden.startswith("<") and forbidden.endswith(">") else None
        parent: dict[Path, Path | None] = {start: None}
        queue = deque([start])
        while queue:
            current = queue.popleft()
            for spelling, resolved in self.edges(current):
                hit = spelling == want_spelling if want_spelling is not None else (
                    resolved is not None and resolved.as_posix() == forbidden)
                if hit:
                    chain = []
                    node: Path | None = current
                    while node is not None:
                        chain.append(node.as_posix())
                        node = parent[node]
                    chain.reverse()
                    chain.append(forbidden)
                    return chain
                if resolved is not None and resolved not in parent:
                    parent[resolved] = current
                    queue.append(resolved)
        return None


def parse_rows(text: str) -> tuple[list[tuple[int, str, str]], list[str]]:
    rows, errors = [], []
    for number, line in enumerate(text.splitlines(), start=1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        match = ROW_RE.match(line)
        if match is None:
            errors.append(f"line {number}: malformed row (want `<header> !<forbidden>`): {line}")
        else:
            rows.append((number, match.group(1), match.group(2)))
    return rows, errors


def check(root: Path, budget_text: str) -> list[str]:
    """Returns the failure messages for `budget_text` evaluated against the tree at `root`."""
    rows, failures = parse_rows(budget_text)
    graph = IncludeGraph(root)
    for number, header, forbidden in rows:
        if not (root / header).is_file():
            failures.append(f"row {number}: no such header: {header}")
            continue
        is_spelling = forbidden.startswith("<") and forbidden.endswith(">")
        if not is_spelling and not (root / forbidden).is_file():
            failures.append(f"row {number}: no such header: {forbidden}")
            continue
        chain = graph.find_chain(normalize(Path(header)), forbidden)
        if chain is not None:
            failures.append(f"row {number}: {header} must not include {forbidden}, but: "
                            + " → ".join(chain))
    return failures


def self_test() -> int:
    failures: list[str] = []

    def expect(label: str, condition: bool) -> None:
        if not condition:
            failures.append(label)

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        files = {
            "src/p/a.hpp": "#pragma once\n#include <p/b.hpp>\n",
            "src/p/b.hpp": "#pragma once\n#if X\n#  include <p/../p/c.hpp>\n#endif\n",
            "src/p/c.hpp": "#pragma once\n#include <QtDBus/QDBusVariant>\n",
            "src/p/clean.hpp": '#pragma once\n#include "leaf.hpp"\n#include <vector>\n',
            "src/p/leaf.hpp": "#pragma once\n",
            "vendor/core-cpp/src/core/v.hpp": "#pragma once\n",
        }
        for name, content in files.items():
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)

        out = check(root, "src/p/a.hpp !src/p/c.hpp\n")
        expect("transitive hit fails", len(out) == 1)
        expect("chain is named",
               bool(out) and "src/p/a.hpp → src/p/b.hpp → src/p/c.hpp" in out[0])

        out = check(root, "src/p/a.hpp !src/p/d.hpp\n")
        expect("missing forbidden fails", len(out) == 1 and "no such header" in out[0])
        out = check(root, "src/p/d.hpp !src/p/c.hpp\n")
        expect("missing subject fails", len(out) == 1 and "no such header" in out[0])

        out = check(root, "src/p/clean.hpp !src/p/c.hpp\n")
        expect("clean row passes", out == [])

        out = check(root, "src/p/a.hpp !<QtDBus/QDBusVariant>\n")
        expect("spelling hit fails with chain",
               len(out) == 1 and "src/p/c.hpp → <QtDBus/QDBusVariant>" in out[0])
        out = check(root, "src/p/clean.hpp !<QtDBus/QDBusVariant>\n")
        expect("spelling clean passes", out == [])

        out = check(root, "src/p/clean.hpp !vendor/core-cpp/src/core/v.hpp\n")
        expect("vendor root resolves and is clean", out == [])
        out = check(root, "# only a comment\n\n")
        expect("empty budget passes", out == [])
        out = check(root, "garbage\n")
        expect("malformed row fails", len(out) == 1 and "malformed" in out[0])

    for failure in failures:
        print(f"check-include-budget self-test: FAILED: {failure}", file=sys.stderr)
    if failures:
        return 1
    print("check-include-budget self-test: OK.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--self-test", action="store_true", help="check the walker on a fixture tree")
    parser.add_argument("--root", type=Path, default=REPO_ROOT, help="repository root (default: this checkout)")
    parser.add_argument("--budget", type=Path, default=BUDGET, help="budget file")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    failures = check(args.root, args.budget.read_text(encoding="utf-8"))
    for failure in failures:
        print(f"check-include-budget: {failure}", file=sys.stderr)
    if failures:
        return 1
    print("check-include-budget: OK.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
