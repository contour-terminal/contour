#! /usr/bin/env python3
"""Spell-checks the repository with `typos`, using the configuration in `_typos.toml`.

This script is the single source of truth for the spell gate: both `ctest -L lint` and the CI job
run *this file*, so a local run and a CI run cannot disagree -- they are the same command, at the
same pinned tool version, against the same config. That property is the whole point; do not add a
second invocation path.

typos is fetched on demand into the build tree if not already on PATH, so this needs no system
install. If it cannot be obtained (offline), the check SKIPS rather than fails. CI sets
CHECK_SPELLING_REQUIRE_TOOL=1 to turn that skip into a hard failure, so a failed download can never
show up as a green gate.

See docs/internals/spell-checking.md for the rationale, including why this is Python.
"""

from __future__ import annotations

import argparse
import sys

from lib.toolcache import REPO_ROOT, ToolSpec, resolve, run

# The pinned version. Because CI runs this same script, this single line keeps local and CI in
# lockstep -- there is no second place to update.
TYPOS_VERSION = "1.48.0"

TYPOS = ToolSpec(
    name="typos",
    version=TYPOS_VERSION,
    url_template=f"https://github.com/crate-ci/typos/releases/download/v{TYPOS_VERSION}/{{asset}}",
    # The Linux builds are static musl binaries, so they carry no runtime dependency. crate-ci
    # publishes no Windows arm64 build, so that host skips.
    assets={
        ("Linux", "x86_64"): f"typos-v{TYPOS_VERSION}-x86_64-unknown-linux-musl.tar.gz",
        ("Linux", "aarch64"): f"typos-v{TYPOS_VERSION}-aarch64-unknown-linux-musl.tar.gz",
        ("Darwin", "x86_64"): f"typos-v{TYPOS_VERSION}-x86_64-apple-darwin.tar.gz",
        ("Darwin", "aarch64"): f"typos-v{TYPOS_VERSION}-aarch64-apple-darwin.tar.gz",
        ("Windows", "x86_64"): f"typos-v{TYPOS_VERSION}-x86_64-pc-windows-msvc.zip",
    },
    require_env="CHECK_SPELLING_REQUIRE_TOOL",
    cache_env="TYPOS_CACHE_DIR",
)

CONFIG_FILE = REPO_ROOT / "_typos.toml"

# A section whose name ends in one of these holds exceptions, and so is subject to the
# justification rule below. Every other section (`[files]`, `[default]`, `[type.cpp-test]`) holds
# globs and patterns, which need no per-line excuse.
EXCEPTION_SECTIONS = ("extend-identifiers]", "extend-words]")


def scan_exceptions(config: str) -> tuple[int, list[tuple[int, str]]]:
    """Counts the exceptions in `_typos.toml` and finds those that do not say why they are there.

    Every exception must carry a justification comment. The previous spell gate accumulated 1121
    unexplained allow-list entries, 86 of which were genuine misspellings that the gate then
    happily accepted -- an unjustified entry is how a word list stops being a word list.

    The comment is required *inline* rather than merely somewhere above: a preceding comment block
    would let a later entry appended beneath it inherit a justification it was never given.

    Counting and checking share this one walk deliberately. They are the same question asked twice
    -- "which lines are exceptions?" -- and a second, section-blind definition would drift from
    this one without ever announcing it.

    @param config Contents of `_typos.toml`, read with newline translation applied so that a CRLF
                  checkout on Windows does not leave a stray carriage return on every line.
    @return The number of exceptions, and (line number, line) for each unjustified one.
    """
    entries = 0
    offenders: list[tuple[int, str]] = []
    in_exception_section = False
    for number, line in enumerate(config.splitlines(), start=1):
        if line.startswith("["):
            in_exception_section = line.rstrip().endswith(EXCEPTION_SECTIONS)
        elif in_exception_section and "=" in line:
            # A comment line needs no separate test: it carries a '#', so it lands in the justified
            # branch below and is not counted -- the same way the awk this replaces behaved.
            entries += 1
            if "#" not in line:
                offenders.append((number, line))
    return entries, offenders


def self_test() -> int:
    """Checks the exception rules against cases they have to get right, without touching the network.

    This parser has been wrong before: an early version keyed off a preceding comment block and so
    accepted a planted misspelling. The rules are the only thing standing between the exception
    list and the 2,237-entry allow-list it replaced, so they are worth a test of their own.

    The fixtures below use placeholder words rather than real misspellings. What is under test is
    purely structural -- it looks at `[`, `=` and `#`, never at the word -- so quoting an actual
    typo here would buy no coverage while making this file trip the very gate it implements.
    """
    # (description, config, expected entry count, expected unjustified line numbers)
    cases: list[tuple[str, str, int, list[int]]] = [
        (
            "an inline comment justifies the entry",
            '[default.extend-words]\nalpha = "alpha" # Justified.\n',
            1,
            [],
        ),
        (
            "a bare entry is rejected",
            '[default.extend-words]\nbravo = "bravo"\n',
            1,
            [2],
        ),
        (
            "a comment block above does not justify what follows it",
            '[default.extend-words]\n# A block comment, covering what comes next.\ncharlie = "charlie"\n',
            1,
            [3],
        ),
        (
            "entries outside an exception section are neither counted nor checked",
            '[files]\nextend-exclude = [\n    "*.desktop",\n]\n',
            0,
            [],
        ),
        (
            "a hyphen-free key outside an exception section is still not an exception",
            '[default]\nlocale = "en-us"\n',
            0,
            [],
        ),
        (
            "a scoped section is checked like the default one",
            '[type.terminfo.extend-words]\ndelta = "delta"\n',
            1,
            [2],
        ),
        (
            "extend-identifiers is checked too",
            '[default.extend-identifiers]\necho = "echo"\n',
            1,
            [2],
        ),
        (
            "leaving an exception section stops both the count and the check",
            '[default.extend-words]\nalpha = "alpha" # Justified.\n'
            '[type.cpp-test]\nextend-glob = ["*_test.cpp"]\n',
            1,
            [],
        ),
        (
            "a trailing carriage return does not hide a section header",
            '[default.extend-words]\r\nfoxtrot = "foxtrot"\r\n',
            1,
            [2],
        ),
    ]

    failures = 0
    for description, config, expected_entries, expected_offenders in cases:
        entries, offenders = scan_exceptions(config)
        actual_offenders = [number for number, _ in offenders]
        if (entries, actual_offenders) != (expected_entries, expected_offenders):
            print(
                f"  FAIL: {description}: expected {expected_entries} entries and offending lines "
                f"{expected_offenders}, got {entries} and {actual_offenders}",
                file=sys.stderr,
            )
            failures += 1

    if failures:
        print(f"check-spelling self-test: {failures} of {len(cases)} cases failed.", file=sys.stderr)
        return 1
    print(f"check-spelling self-test: {len(cases)} cases OK.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="check the exception rules against known cases and exit; needs no network",
    )
    arguments = parser.parse_args()

    if arguments.self_test:
        return self_test()

    # Text mode, so universal newlines fold a CRLF checkout back to '\n' before anything parses it.
    entries, offenders = scan_exceptions(CONFIG_FILE.read_text(encoding="utf-8"))
    if offenders:
        print("_typos.toml: these exceptions have no justification comment:", file=sys.stderr)
        for number, line in offenders:
            print(f"  {CONFIG_FILE.name}:{number}: {line}", file=sys.stderr)
        print("Say why the word is not a typo, or fix the text instead of excusing it.", file=sys.stderr)
        return 1

    typos = resolve(TYPOS)

    # No path arguments: typos walks the whole tree under `_typos.toml`, honouring .gitignore.
    print(f"Running spell check with typos v{TYPOS_VERSION}...")
    exit_code = run(typos, [])
    if exit_code != 0:
        return exit_code

    print(f"typos: spelling OK ({entries} exceptions, all justified).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
