#! /usr/bin/env python3
"""Plans a clang-tidy run over the lines a diff changed.

    tidy-diff-plan.py --build-dir DIR --diff FILE [--root DIR] [--tsv]
    tidy-diff-plan.py --self-test

Reads a `git diff -U0` and prints, as JSON, one entry per translation unit that has to be linted:

    [{"tu": "<abs .cpp>", "lines": {"<abs file>": [[start, end], ...]}}]

* A changed first-party .cpp that is in DIR/compile_commands.json lints itself.
* A changed .hpp/.h under src/ cannot be linted on its own, so it is attached to ONE includer TU:
  the one with the fewest dependencies. Dependencies come from `ninja -C DIR -t deps` when the tree
  has been built, and from a scan of the first-party `#include` lines otherwise, so a tree that was
  only configured (the PR job builds nothing but generated headers) plans identically.
  The scan cannot see `#if`, so each chosen (TU, header) pair is checked with the TU's own compile
  command in preprocessor-only dependency mode (`-M`); a TU that does not really reach the header is
  passed over for the next includer. Ninja's record is ground truth and needs no check.
* A changed header that no TU reaches is an error (exit 2): a change must never be skipped silently.
  The exception is a header whose path names another OS (PLATFORM_MARKERS): it is noted on stderr.
* A changed file under src/ in a C-family language this planner does not handle (.mm, .c, ...) is noted.
* Known limitation, shared with the full-tree tidy: lines inside an inactive `#if` region of the
  header itself are not analysed, so they cannot be reported.
* A changed .cpp that is not in the compile database (another OS's file) is noted on stderr and skipped.

Produce the diff with: git diff -U0 --no-color --no-ext-diff --src-prefix=a/ --dst-prefix=b/ BASE
(scripts/clang-tidy-diff.sh does), so user git configuration cannot change its shape.

With --tsv each line is `<tu>TAB<clang-tidy -line-filter JSON>` instead, ready for a shell loop.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile
from collections import defaultdict
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SOURCE_SUFFIXES = (".cpp", ".cc", ".cxx")
HEADER_SUFFIXES = (".hpp", ".h")
EXIT_UNPLANNABLE = 2
MAX_VERIFIED_CANDIDATES = 12
# C-family files this planner does not lint; a change to one is reported rather than ignored.
UNHANDLED_CODE_SUFFIXES = (".mm", ".m", ".c", ".inl", ".ipp", ".tpp")
# Path fragments that mark a file as belonging to another OS (derived from the files under src/ that
# the Linux compile database leaves out: Cocoa*, CoreText*, DirectWrite*, Win32*, ConPty*, *_win32.*).
# A header matching one and reached by no TU is not an error, because no Linux TU can include it.
PLATFORM_MARKERS = (
    re.compile(r"Cocoa|CoreText|DirectWrite|Win32|ConPty", re.IGNORECASE),
    re.compile(r"_(win32|windows|macos|darwin)\b", re.IGNORECASE),
    re.compile(r"/(win32|windows|macos|darwin)/", re.IGNORECASE),
)
HUNK_RE = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@")
INCLUDE_RE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]', re.MULTILINE)
INCLUDE_DIR_FLAGS = ("-I", "-isystem", "-iquote")

Ranges = dict[str, list[list[int]]]


class UnplannableError(Exception):
    """A changed file that the plan cannot cover."""


def parse_diff(text: str, root: Path) -> Ranges:
    """Maps each changed file (absolute path) to its added/changed line ranges; deletions yield none."""
    result: Ranges = {}
    current: str | None = None
    for line in text.splitlines():
        if line.startswith("+++ "):
            target = line[4:].split("\t")[0]
            if target == "/dev/null":
                current = None
            else:
                current = str(root / target.removeprefix("b/"))
            continue
        match = HUNK_RE.match(line)
        if match and current is not None:
            start = int(match.group(1))
            count = int(match.group(2)) if match.group(2) is not None else 1
            if count > 0:
                result.setdefault(current, []).append([start, start + count - 1])
    return result


def load_compile_db(build_dir: Path) -> list[dict]:
    return json.loads((build_dir / "compile_commands.json").read_text())


def split_command(command: str, windows: bool = os.name == "nt") -> list[str]:
    """Splits a compile-database `command`; POSIX rules would eat the backslashes of Windows paths."""
    return shlex.split(command, posix=not windows)


def entry_arguments(entry: dict) -> list[str]:
    return entry["arguments"] if "arguments" in entry else split_command(entry["command"])


def include_dirs_of(entry: dict) -> list[Path]:
    arguments = entry_arguments(entry)
    base = Path(entry["directory"])
    found: list[Path] = []
    for index, argument in enumerate(arguments):
        for flag in INCLUDE_DIR_FLAGS:
            if argument == flag and index + 1 < len(arguments):
                found.append(base / arguments[index + 1])
            elif argument.startswith(flag) and argument != flag and flag == "-I":
                found.append(base / argument[len(flag):])
    return found


def is_first_party(path: Path, root: Path) -> bool:
    try:
        relative = path.relative_to(root)
    except ValueError:
        return False
    return relative.parts[:1] == ("src",) and not any(
        part.endswith("_autogen") or part == "_deps" for part in relative.parts
    )


def parse_ninja_deps(text: str, build_dir: Path) -> dict[Path, list[Path]]:
    """Parses `ninja -t deps`: object file -> dependencies, both resolved against the build dir."""
    deps: dict[Path, list[Path]] = {}
    current: list[Path] | None = None
    for line in text.splitlines():
        if line.startswith((" ", "\t")):
            if current is not None and line.strip():
                current.append((build_dir / line.strip()).resolve())
        elif ": #deps" in line:
            obj = line.split(": #deps")[0]
            current = deps.setdefault((build_dir / obj).resolve(), [])
        else:
            current = None
    return deps


def ninja_deps(build_dir: Path) -> str:
    try:
        run = subprocess.run(["ninja", "-C", str(build_dir), "-t", "deps"],
                             capture_output=True, text=True, check=False)
    except OSError:
        return ""
    return run.stdout if run.returncode == 0 else ""


class IncludeGraph:
    """Direct-include edges between first-party files, from a regex scan of `#include` lines."""

    def __init__(self, root: Path, include_dirs: list[Path]) -> None:
        self._root = root
        resolved = dict.fromkeys(d.resolve() for d in include_dirs)  # ordered and deduplicated
        self._dirs = [d for d in resolved if is_first_party(d, root)]
        self._edges: dict[Path, list[Path]] = {}

    def _resolve(self, includer: Path, kind: str, name: str) -> Path | None:
        candidates = [includer.parent] if kind == '"' else []
        for directory in [*candidates, *self._dirs]:
            candidate = (directory / name).resolve()
            if candidate.is_file() and is_first_party(candidate, self._root):
                return candidate
        return None

    def includes(self, path: Path) -> list[Path]:
        if path not in self._edges:
            try:
                source = path.read_text(errors="replace")
            except OSError:
                source = ""
            resolved = (self._resolve(path, kind, name) for kind, name in INCLUDE_RE.findall(source))
            self._edges[path] = [p for p in resolved if p is not None]
        return self._edges[path]

    def closure(self, path: Path) -> set[Path]:
        seen: set[Path] = set()
        stack = [path]
        while stack:
            for dependency in self.includes(stack.pop()):
                if dependency not in seen:
                    seen.add(dependency)
                    stack.append(dependency)
        return seen


PREPROCESSOR_DIAGNOSTIC_LINES = 6


def real_dependencies(entry: dict, log=lambda message: None) -> set[Path] | None:
    """What the compiler really reads for this TU: its own command run as `-M` (preprocess only).

    Honours the TU's -D/-I flags, so `#if` regions are resolved as in a real build. None when the
    preprocessor fails (e.g. a generated header is missing).
    """
    arguments = entry_arguments(entry)
    kept: list[str] = []
    skip_next = False
    for argument in arguments:
        if skip_next:
            skip_next = False
        elif argument in ("-o", "-MF", "-MT", "-MQ"):
            skip_next = True
        elif argument in ("-c", "-MD", "-MMD") or argument.startswith(("-MF", "-MT", "-MQ")):
            continue
        else:
            kept.append(argument)
    while kept and Path(kept[0]).name in ("ccache", "sccache", "pace-cc", "distcc"):
        kept.pop(0)
    try:
        run = subprocess.run([*kept, "-M"], capture_output=True, text=True, check=False,
                             cwd=entry["directory"])
    except OSError as error:
        log(f"tidy-diff-plan: cannot run {kept[0]} for {entry['file']}: {error}")
        return None
    if run.returncode != 0:
        first_lines = run.stderr.strip().splitlines()[:PREPROCESSOR_DIAGNOSTIC_LINES]
        log(f"tidy-diff-plan: `-M` of {entry['file']} exited {run.returncode}:\n  "
            + "\n  ".join(first_lines or ["(no diagnostics on stderr)"]))
        return None
    base = Path(entry["directory"])
    tokens = run.stdout.replace("\\\n", " ").split()
    return {(base / token).resolve() for token in tokens[1:] if not token.endswith(":")}


def pick_includer(header: Path, tus: list[Path], entries: dict[Path, dict],
                  deps_by_tu: dict[Path, list[Path]] | None, graph: IncludeGraph,
                  verify, log) -> Path | None:
    """The TU including `header` with the fewest dependencies, or None if no TU really reaches it.

    Ninja's record wins. Otherwise candidates come from the include scan, fewest first, and each is
    confirmed by `verify` (a TU's real preprocessor dependencies) before it is trusted.
    """
    if deps_by_tu:
        ranked = sorted((len(deps), str(tu)) for tu, deps in deps_by_tu.items() if header in set(deps))
        if ranked:
            return Path(ranked[0][1])
    scanned = sorted((len(closure), str(tu)) for tu in tus
                     if header in (closure := graph.closure(tu)))
    for _, name in scanned[:MAX_VERIFIED_CANDIDATES]:
        tu = Path(name)
        reached = verify(entries[tu], log)
        if reached is None:
            log(f"tidy-diff-plan: {tu}: preprocessing failed; cannot use it for {header}")
        elif header in reached:
            return tu
    return None


def is_other_platform(path: Path, root: Path) -> bool:
    relative = "/" + path.relative_to(root).as_posix() if path.is_relative_to(root) else str(path)
    return any(marker.search(relative) for marker in PLATFORM_MARKERS)


def merge_ranges(ranges: list[list[int]]) -> list[list[int]]:
    merged: list[list[int]] = []
    for start, end in sorted(ranges):
        if merged and start <= merged[-1][1] + 1:
            merged[-1][1] = max(merged[-1][1], end)
        else:
            merged.append([start, end])
    return merged


def plan(root: Path, build_dir: Path, diff_text: str, deps_text: str | None, log,
         verify=real_dependencies) -> list[dict]:
    """Builds the plan; raises UnplannableError for a header no TU includes."""
    root = root.resolve()
    db = load_compile_db(build_dir)
    tu_entries = {Path(e["file"]).resolve(): e for e in db
                  if e["file"].endswith(SOURCE_SUFFIXES)}
    first_party_tus = sorted(tu for tu in tu_entries if is_first_party(tu, root))
    include_dirs = [d for tu in first_party_tus for d in include_dirs_of(tu_entries[tu])]
    graph = IncludeGraph(root, include_dirs)

    deps_by_tu: dict[Path, list[Path]] | None = None
    if deps_text:
        object_to_tu = {(Path(e["directory"]) / e["output"]).resolve(): tu
                        for tu, e in tu_entries.items() if "output" in e and tu in set(first_party_tus)}
        parsed = parse_ninja_deps(deps_text, build_dir.resolve())
        deps_by_tu = {object_to_tu[obj]: deps for obj, deps in parsed.items() if obj in object_to_tu}

    attached: dict[Path, Ranges] = defaultdict(dict)
    unplannable: list[str] = []
    for name, ranges in sorted(parse_diff(diff_text, root).items()):
        path = Path(name)
        if not is_first_party(path, root.resolve()) and not is_first_party(path.resolve(), root):
            continue
        if path.suffix in SOURCE_SUFFIXES:
            if path.resolve() in tu_entries:
                attached[path.resolve()].setdefault(str(path), []).extend(ranges)
            else:
                log(f"tidy-diff-plan: {name}: not in compile_commands.json "
                    "(another platform's file?); not linted")
        elif path.suffix in HEADER_SUFFIXES:
            includer = pick_includer(path.resolve(), first_party_tus, tu_entries, deps_by_tu, graph,
                                     verify, log)
            if includer is None and is_other_platform(path, root):
                log(f"tidy-diff-plan: not linted on this platform: {name}")
            elif includer is None:
                unplannable.append(name)
            else:
                attached[includer].setdefault(str(path), []).extend(ranges)
        elif path.suffix in UNHANDLED_CODE_SUFFIXES:
            log(f"tidy-diff-plan: {name}: {path.suffix} files are not linted by this tool")
    if unplannable:
        raise UnplannableError("changed header(s) reached by no translation unit in "
                               "compile_commands.json, so their lines cannot be linted: "
                               + ", ".join(unplannable))
    return [{"tu": str(tu), "lines": {f: merge_ranges(r) for f, r in sorted(files.items())}}
            for tu, files in sorted(attached.items())]


def filter_name(path: str) -> str:
    """The name clang-tidy's -line-filter matches a file against: its last two path components.

    clang-tidy compares the filter name as a suffix of the file name *as the compiler spelled it*,
    and an include such as `-I src/contour` + `<../contour/window/X.hpp>` spells it
    `src/contour/../contour/window/X.hpp`. The absolute path is no suffix of that, so the lines would
    be dropped without a word. `window/X.hpp` is a suffix of every spelling of the same file, and
    can only over-match another file of that name and directory name, never under-match.
    """
    return "/".join(Path(path).parts[-2:])


def line_filter(lines: Ranges) -> str:
    return json.dumps([{"name": filter_name(name), "lines": ranges} for name, ranges in lines.items()],
                      separators=(",", ":"))


def self_test() -> int:
    failures: list[str] = []

    def check(label: str, condition: bool) -> None:
        if not condition:
            failures.append(label)

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp).resolve()
        build = root / "out" / "b"
        files = {
            "src/a/X.hpp": "#pragma once\n",
            "src/a/Y.hpp": '#pragma once\n#include <a/X.hpp>\n',
            "src/a/Orphan.hpp": "#pragma once\n",
            "src/a/A.cpp": '#include <a/X.hpp>\n',
            "src/a/B.cpp": '#include "Y.hpp"\n#include <vector>\n',
            "src/a/OtherOs.cpp": "int x;\n",
        }
        for name, content in files.items():
            (root / name).parent.mkdir(parents=True, exist_ok=True)
            (root / name).write_text(content)
        build.mkdir(parents=True)
        db = [{"directory": str(build), "file": str(root / f"src/a/{n}.cpp"),
               "output": f"src/a/CMakeFiles/a.dir/{n}.cpp.o",
               "arguments": ["clang++", f"-I{root / 'src'}", "-isystem", "/usr/include", "-c",
                             str(root / "src" / "a" / f"{n}.cpp")]}
              for n in ("A", "B")]
        (build / "compile_commands.json").write_text(json.dumps(db))

        def diff(path: str, *hunks: str) -> str:
            return f"--- a/{path}\n+++ b/{path}\n" + "".join(f"{h}\n" for h in hunks)

        # What each TU "really" preprocesses; stands in for `-M`, since #if is invisible to the scan.
        reached: dict[str, set[Path]] = {}

        def verify(entry: dict, log=None) -> set[Path] | None:
            tu = Path(entry["file"])
            return reached[tu.name] if tu.name in reached else graph_of(tu)

        def graph_of(tu: Path) -> set[Path]:
            fixture_graph = IncludeGraph(root, [root / "src"])
            return {tu, *fixture_graph.closure(tu)}

        def run(diff_text: str, deps: str | None) -> tuple[list[dict] | None, list[str]]:
            notes: list[str] = []
            try:
                return plan(root, build, diff_text, deps, notes.append, verify), notes
            except UnplannableError as error:
                notes.append(str(error))
                return None, notes

        x = str(root / "src/a/X.hpp")
        a = str(root / "src/a/A.cpp")
        fake_deps = (
            "src/a/CMakeFiles/a.dir/A.cpp.o: #deps 3, deps mtime 1 (VALID)\n"
            f"    {root}/src/a/A.cpp\n    {x}\n    /usr/include/stdio.h\n\n"
            "src/a/CMakeFiles/a.dir/B.cpp.o: #deps 9, deps mtime 1 (VALID)\n"
            f"    {root}/src/a/B.cpp\n    {x}\n    {root}/src/a/Y.hpp\n\n"
        )
        # (a) the includer with the fewest dependencies wins, from ninja deps...
        result, _ = run(diff("src/a/X.hpp", "@@ -9,0 +10,3 @@"), fake_deps)
        check("a/ninja", result == [{"tu": a, "lines": {x: [[10, 12]]}}])
        # ...and from the include scan when the tree was never built (A closure 1, B closure 2).
        result, _ = run(diff("src/a/X.hpp", "@@ -9,0 +10,3 @@"), None)
        check("a/scan", result == [{"tu": a, "lines": {x: [[10, 12]]}}])
        # The scan follows nested includes: Y.hpp is reached only through B.cpp.
        result, _ = run(diff("src/a/Y.hpp", "@@ -1 +2 @@"), None)
        check("a/transitive", result == [{"tu": str(root / "src/a/B.cpp"),
                                          "lines": {str(root / "src/a/Y.hpp"): [[2, 2]]}}])
        # A .cpp and a header landing on the same TU share one entry, with adjacent ranges merged.
        both = diff("src/a/A.cpp", "@@ -1 +1 @@") + diff("src/a/X.hpp", "@@ -1 +2,2 @@", "@@ -5 +5 @@")
        result, _ = run(both, None)
        check("a/merge", result is not None and len(result) == 1
              and result[0]["lines"] == {a: [[1, 1]], x: [[2, 3], [5, 5]]})
        # (b) a .cpp missing from the compile database is skipped, loudly.
        result, notes = run(diff("src/a/OtherOs.cpp", "@@ -0,0 +1 @@"), None)
        check("b/ignored", result == [] and any("OtherOs.cpp" in n for n in notes))
        # (c) a header nothing includes is an error, never a skip.
        result, notes = run(diff("src/a/Orphan.hpp", "@@ -0,0 +1 @@"), None)
        check("c/unplannable", result is None and any("Orphan.hpp" in n for n in notes))
        # (d) deleted files and pure-deletion hunks contribute no lines.
        deleted = "--- a/src/a/Gone.hpp\n+++ /dev/null\n@@ -1,3 +0,0 @@\n"
        result, _ = run(deleted + diff("src/a/Orphan.hpp", "@@ -4,2 +3,0 @@"), None)
        check("d/deletions", result == [])
        # Files outside src/ (vendor, docs) are not ours to lint.
        result, _ = run(diff("vendor/core-cpp/x.hpp", "@@ -1 +1 @@") + diff("README.md", "@@ -1 +1 @@"), None)
        check("e/non-first-party", result == [])
        # Importance 1: the scan's best includer (A.cpp) hides X.hpp behind an inactive #if, so the
        # next candidate (B.cpp, via Y.hpp) is used; if none reaches it the header is unplannable.
        reached["A.cpp"] = {root / "src/a/A.cpp"}
        result, _ = run(diff("src/a/X.hpp", "@@ -9,0 +10,3 @@"), None)
        check("g/next-candidate", result == [{"tu": str(root / "src/a/B.cpp"), "lines": {x: [[10, 12]]}}])
        reached["B.cpp"] = {root / "src/a/B.cpp"}
        result, notes = run(diff("src/a/X.hpp", "@@ -9,0 +10,3 @@"), None)
        check("g/none-reaches", result is None and any("X.hpp" in n for n in notes))
        # ...but ninja's record is ground truth and is not second-guessed.
        result, _ = run(diff("src/a/X.hpp", "@@ -9,0 +10,3 @@"), fake_deps)
        check("g/ninja-unverified", result == [{"tu": a, "lines": {x: [[10, 12]]}}])
        reached.clear()
        # Another OS's header that no Linux TU reaches is noted, not fatal; an ordinary orphan still is.
        (root / "src/a/CocoaThing.hpp").write_text("#pragma once\n")
        result, notes = run(diff("src/a/CocoaThing.hpp", "@@ -0,0 +1 @@"), None)
        check("h/other-os", result == [] and any("not linted on this platform" in n for n in notes))
        check("h/markers", is_other_platform(Path("/r/src/x/Foo_win32.hpp"), Path("/r"))
              and is_other_platform(Path("/r/src/x/win32/Foo.hpp"), Path("/r"))
              and not is_other_platform(Path("/r/src/a/Orphan.hpp"), Path("/r")))
        # Minor: a C-family file this tool cannot lint is reported.
        result, notes = run(diff("src/a/Thing.mm", "@@ -1 +1 @@"), None)
        check("i/unhandled-suffix", result == [] and any("Thing.mm" in n for n in notes))
        check("f/line-filter", json.loads(line_filter({x: [[1, 2]]})) == [{"name": "a/X.hpp", "lines": [[1, 2]]}])
        spelled = "src/contour/../contour/window/X.hpp"  # how clang spells a header reached via `../`
        # A Windows `command` string keeps its backslashes (POSIX splitting would swallow them).
        windows_command = r"clang++ -IC:\work\src -c C:\work\src\a\A.cpp"
        check("j/windows-split", split_command(windows_command, windows=True)
              == ["clang++", r"-IC:\work\src", "-c", r"C:\work\src\a\A.cpp"])
        check("j/posix-split", split_command("clang++ -I/w/src 'a b.cpp'", windows=False)
              == ["clang++", "-I/w/src", "a b.cpp"])
        check("f/suffix", spelled.endswith(filter_name("/repo/src/contour/window/X.hpp")))

    for failure in failures:
        print(f"tidy-diff-plan self-test: FAILED: {failure}", file=sys.stderr)
    if failures:
        return 1
    print("tidy-diff-plan self-test: OK.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--self-test", action="store_true", help="check the planner on fixture trees")
    parser.add_argument("--build-dir", type=Path, help="build tree holding compile_commands.json")
    parser.add_argument("--diff", type=Path, help="a `git diff -U0` to plan for")
    parser.add_argument("--root", type=Path, default=REPO_ROOT, help="repository root (default: this checkout)")
    parser.add_argument("--tsv", action="store_true", help="print `<tu>TAB<line-filter JSON>` lines instead of JSON")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if args.build_dir is None or args.diff is None:
        parser.error("--build-dir and --diff are required (or --self-test)")

    def log(message: str) -> None:
        print(message, file=sys.stderr)

    try:
        entries = plan(args.root, args.build_dir.resolve(), args.diff.read_text(),
                       ninja_deps(args.build_dir), log)
    except UnplannableError as error:
        log(f"tidy-diff-plan: {error}")
        return EXIT_UNPLANNABLE
    if args.tsv:
        for entry in entries:
            print(f"{entry['tu']}\t{line_filter(entry['lines'])}")
    else:
        print(json.dumps(entries, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
