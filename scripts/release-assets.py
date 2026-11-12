#! /usr/bin/env python3
"""Selects the files a release attaches, from the list in .github/release.json.

    release-assets.py                 check that release.json is well formed, and that it has a row for every
                                      version-stamped package the workflow uploads
    release-assets.py select DIR      print the one file under DIR for each pattern, in pattern order
    release-assets.py --self-test     check the selection rules against inline samples

Each pattern is an fnmatch file-name pattern with `[0-9]*` exactly once, where the version goes. That
spelling keeps `contour-[0-9]*-ubuntu26.04-amd64.deb` from also matching `contour-daemon-...`, because
`d` is not a digit. A pattern must match exactly one file; files no pattern claims are only reported.

release.json is also read outside this repository: the owner's release tooling deep-merges the whole
file over the release model it detects, so a top-level key that collides with a name of that model
(trigger, changelog, tagPrefix, ...) silently overrides it. Add no key besides `artifacts` without
checking.

Python rather than shell so that CTest runs it, and so it runs on Windows too.
"""

from __future__ import annotations

import argparse
import fnmatch
import json
import re
import sys
from collections.abc import Iterable, Sequence
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

REPO_ROOT = Path(__file__).resolve().parent.parent
RELEASE_JSON = REPO_ROOT / ".github" / "release.json"
WORKFLOW = REPO_ROOT / ".github" / "workflows" / "build.yml"
VERSION_WILDCARD = "[0-9]*"
_SAMPLE_VERSION = "0.0.0.0"


@dataclass(frozen=True)
class Selection:
    """The outcome of matching patterns against file names."""

    files: list[str]
    unclaimed: list[str]
    problems: list[str]


def select(patterns: Sequence[str], names: Iterable[str]) -> Selection:
    """Picks, per pattern and in pattern order, the single name whose basename matches it."""
    names = list(names)
    files: list[str] = []
    problems: list[str] = []
    claimed: set[str] = set()
    for pattern in patterns:
        matches = [n for n in names if fnmatch.fnmatchcase(PurePosixPath(n).name, pattern)]
        claimed.update(matches)
        if not matches:
            problems.append(f"no file matches {pattern}")
        elif len(matches) > 1:
            problems.append(f"{len(matches)} files match {pattern}: {', '.join(matches)}")
        else:
            files.append(matches[0])
    return Selection(files, [n for n in names if n not in claimed], problems)


# The spellings of a separately published debug-symbol package: Ubuntu's -dbgsym .ddeb, Fedora's -debuginfo and
# -debugsource, the Windows PDB zip and the macOS dSYM zip. No build produces one any more.
_SYMBOL_PACKAGE = re.compile(r"dbgsym|\.ddeb$|debuginfo|debugsource|-pdb\.|dSYM")


def pattern_problems(patterns: Sequence[str]) -> list[str]:
    """Returns what is wrong with the pattern list itself, including rows that could claim the same file."""
    if not patterns:
        return ["the artifact list is empty"]
    problems: list[str] = []
    for index, pattern in enumerate(patterns):
        if pattern in patterns[:index]:
            problems.append(f"duplicate pattern {pattern}")
        elif _SYMBOL_PACKAGE.search(pattern):
            problems.append(f"pattern {pattern} names a debug-symbol package, which no build produces")
        elif pattern.count(VERSION_WILDCARD) != 1:
            problems.append(f"pattern {pattern} must contain {VERSION_WILDCARD} exactly once")
    if problems:
        return problems
    # The wildcard's `*` also crosses hyphens, so one row can swallow another's file name.
    for index, pattern in enumerate(patterns):
        sample = pattern.replace(VERSION_WILDCARD, _SAMPLE_VERSION)
        problems += [
            f"patterns {pattern} and {other} both match {sample}"
            for other in patterns[index + 1 :]
            if fnmatch.fnmatchcase(sample, other)
        ] + [
            f"patterns {other} and {pattern} both match {sample}"
            for other in patterns[:index]
            if fnmatch.fnmatchcase(sample, other)
        ]
    return problems


def load_patterns(path: Path) -> list[str]:
    """Reads the `artifacts` list of release.json; raises ValueError when it is not a list of strings."""
    document = json.loads(path.read_text(encoding="utf-8"))
    artifacts = document.get("artifacts") if isinstance(document, dict) else None
    if not isinstance(artifacts, list) or not all(isinstance(entry, str) for entry in artifacts):
        raise ValueError(f"{path.name} needs an `artifacts` list of strings")
    return artifacts


# An artifact upload whose name is `contour-<version>-...` with the version spelled as a step output. Names
# with a matrix variable in them (the per-release packages) are not checked here.
_UPLOAD_NAME = re.compile(
    r'^\s+name: "?(contour-\$\{\{ steps\.set_vars\.outputs\.(?:version|VERSION_STRING) \}\}-[^"\s$]*)"?\s*$',
    re.MULTILINE,
)
_VERSION_EXPRESSION = re.compile(r"\$\{\{ steps\.set_vars\.outputs\.(?:version|VERSION_STRING) \}\}")


def upload_problems(patterns: Sequence[str], workflow: str) -> list[str]:
    """Names the version-stamped artifacts a workflow uploads that no pattern would attach to a release.

    A package row missing from release.json is otherwise silent: files no pattern claims are only
    reported when a release is assembled, and the release then simply lacks them.
    """
    problems = []
    for expression in _UPLOAD_NAME.findall(workflow):
        name = _VERSION_EXPRESSION.sub(_SAMPLE_VERSION, expression)
        if not any(fnmatch.fnmatchcase(name, pattern) for pattern in patterns):
            problems.append(f"the workflow uploads {name}, which no row of release.json attaches to a release")
    return problems


_VERSION = "0.9.0.9500"
_PATTERNS = [
    "contour-[0-9]*-ubuntu26.04-amd64.deb",
    "contour-daemon-[0-9]*-ubuntu26.04-amd64.deb",
    "contour-[0-9]*-macOS-arm.dmg",
    "contour-[0-9]*-win64.msi",
    "contour-[0-9]*-win64.zip",
]


def _names(*, skip: str = "") -> list[str]:
    return [p.replace(VERSION_WILDCARD, _VERSION) for p in _PATTERNS if p != skip]


_WIN_ZIP = "contour-[0-9]*-win64.zip"
_WIN_MSI = "contour-[0-9]*-win64.msi"
_GUI_DEB = "contour-[0-9]*-ubuntu26.04-amd64.deb"
_RAWHIDE = f"contour-{_VERSION}-fedorarawhide-x86_64.rpm"
_USER_MSI = f"contour-{_VERSION}-win64-user.msi"

# (description, patterns, names, expected files, expected unclaimed, expected problems)
_SELECT_CASES: list[tuple[str, list[str], list[str], list[str], list[str], list[str]]] = [
    ("one file per pattern, in pattern order", _PATTERNS, _names(), _names(), [], []),
    (
        "a missing dmg is a problem",
        _PATTERNS,
        _names(skip="contour-[0-9]*-macOS-arm.dmg"),
        [n for n in _names() if not n.endswith(".dmg")],
        [],
        ["no file matches contour-[0-9]*-macOS-arm.dmg"],
    ),
    (
        "two runs' msi files name the pattern and both files",
        _PATTERNS,
        [*_names(), "contour-0.9.0.9499-win64.msi"],
        [n for n in _names() if not n.endswith(".msi")],
        [],
        [
            f"2 files match {_WIN_MSI}: contour-{_VERSION}-win64.msi, contour-0.9.0.9499-win64.msi",
        ],
    ),
    (
        "the daemon deb is not the GUI deb",
        [_GUI_DEB],
        [f"contour-daemon-{_VERSION}-ubuntu26.04-amd64.deb"],
        [],
        [f"contour-daemon-{_VERSION}-ubuntu26.04-amd64.deb"],
        [f"no file matches {_GUI_DEB}"],
    ),
    (
        "the user msi is not claimed by the win64 patterns",
        _PATTERNS,
        [*_names(), _USER_MSI],
        _names(),
        [_USER_MSI],
        [],
    ),
    ("a rawhide rpm is unclaimed, not a problem", _PATTERNS, [*_names(), _RAWHIDE], _names(), [_RAWHIDE], []),
    (
        "files are matched by basename",
        _PATTERNS,
        [f"dir/sub/{n}" for n in _names()],
        [f"dir/sub/{n}" for n in _names()],
        [],
        [],
    ),
]

_SAMPLE_UPLOADS = """
      - uses: actions/upload-artifact@v4
        with:
          name: "contour-${{ steps.set_vars.outputs.version }}-win64.zip"
      - uses: actions/upload-artifact@v4
        with:
          name: "contour-${{ steps.set_vars.outputs.VERSION_STRING }}-ubuntu${{ matrix.os_version }}-amd64.deb"
      - uses: actions/upload-artifact@v4
        with:
          name: chrome-snapshots
"""

_SAMPLE_PDB_UPLOAD = """
      - uses: actions/upload-artifact@v4
        with:
          name: "contour-${{ steps.set_vars.outputs.version }}-win64-pdb.zip"
"""

# (description, patterns, workflow text, expected number of problems)
_UPLOAD_CASES: list[tuple[str, list[str], str, int]] = [
    ("every uploaded package has a row", ["contour-[0-9]*-win64.zip"], _SAMPLE_UPLOADS, 0),
    ("an uploaded symbol archive without a row", ["contour-[0-9]*-win64.zip"], _SAMPLE_UPLOADS + _SAMPLE_PDB_UPLOAD, 1),
    ("no upload names are found in an empty workflow", ["contour-[0-9]*-win64.zip"], "", 0),
]

# (description, patterns, expected number of problems)
_PATTERN_CASES: list[tuple[str, list[str], int]] = [
    ("an empty list", [], 1),
    ("a duplicate pattern", ["a-[0-9]*.msi", "a-[0-9]*.msi"], 1),
    ("a pattern without a version wildcard", ["*.msi"], 1),
    ("a pattern with two version wildcards", ["a-[0-9]*-[0-9]*.msi"], 1),
    ("rows that can claim the same file", ["a-[0-9]*-win64.zip", "a-[0-9]*-portable-win64.zip"], 1),
    ("well-formed, disjoint patterns", _PATTERNS, 0),
    ("a Windows PDB zip row", ["a-[0-9]*-win64.zip", "a-[0-9]*-win64-pdb.zip"], 1),
    ("a macOS dSYM zip row", ["a-[0-9]*-macOS-arm.dmg", "a-[0-9]*-macOS-arm-dSYM.zip"], 1),
    ("an Ubuntu dbgsym .ddeb row", ["a-[0-9]*-amd64.deb", "a-dbgsym-[0-9]*-amd64.ddeb"], 1),
    ("a Fedora debuginfo rpm row", ["a-[0-9]*-x86_64.rpm", "a-debuginfo-[0-9]*-x86_64.rpm"], 1),
    ("a Fedora debugsource rpm row", ["a-[0-9]*-x86_64.rpm", "a-debugsource-[0-9]*-x86_64.rpm"], 1),
]


def self_test() -> int:
    """Runs every case; returns the process exit code."""
    failures: list[str] = []
    for description, patterns, names, files, unclaimed, problems in _SELECT_CASES:
        got = select(patterns, names)
        if (got.files, got.unclaimed, got.problems) != (files, unclaimed, problems):
            failures.append(f"{description}: expected {(files, unclaimed, problems)}, got {got}")
    for description, patterns, count in _PATTERN_CASES:
        found = pattern_problems(patterns)
        if len(found) != count:
            failures.append(f"{description}: expected {count} problem(s), got {found}")
    for description, patterns, workflow, count in _UPLOAD_CASES:
        found = upload_problems(patterns, workflow)
        if len(found) != count:
            failures.append(f"{description}: expected {count} problem(s), got {found}")
    for failure in failures:
        print(f"release-assets self-test: FAILED: {failure}", file=sys.stderr)
    if failures:
        return 1
    print(f"release-assets self-test: {len(_SELECT_CASES) + len(_PATTERN_CASES) + len(_UPLOAD_CASES)} cases OK.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--self-test", action="store_true", help="check the rules against inline samples")
    commands = parser.add_subparsers(dest="command")
    select_parser = commands.add_parser("select", help="print the file for each pattern")
    select_parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    if args.self_test:
        return self_test()

    try:
        patterns = load_patterns(RELEASE_JSON)
    except (OSError, ValueError) as error:
        print(f"[RELEASE ASSETS FAILED] cannot read {RELEASE_JSON.name}: {error}", file=sys.stderr)
        return 1
    problems = pattern_problems(patterns)
    if args.command == "select":
        paths = [p.as_posix() for p in args.directory.rglob("*") if p.is_file()]
        selection = select(patterns, sorted(paths))
        problems += selection.problems
        for name in selection.unclaimed:
            print(f"release-assets: notice: not attached: {name}", file=sys.stderr)
        if not problems:
            print("\n".join(selection.files))
    else:
        problems += upload_problems(patterns, WORKFLOW.read_text(encoding="utf-8"))
        if not problems:
            print(f"release-assets: {len(patterns)} patterns OK.")
    for problem in problems:
        print(f"[RELEASE ASSETS FAILED] {problem}", file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
