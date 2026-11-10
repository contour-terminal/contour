#! /usr/bin/env python3
"""Keeps the PTY keeper free of the terminal engine.

The keeper holds every shell's PTY so that the shells survive a crash of the daemon; it can only do that
if nothing the daemon crashes in is linked into it. Its sources (src/vthost/keeper/) therefore include
none of <vtbackend/...>, <vtparser/...>, <vtworkspace/...> and, of vthost, only its own <vthost/keeper/...>
and the dependency-free <vthost/proto/Wire.hpp>. Every offending line is printed as file:line.
`--self-test` checks the pattern against inline fixtures and needs no tree. Python rather than shell, so
that ctest runs it on every platform.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
KEEPER_ROOT = Path("src/vthost/keeper")

FORBIDDEN = re.compile(
    r'^\s*#\s*include\s*[<"](?:vtbackend/|vtparser/|vtworkspace/'
    r"|vthost/(?!keeper/|proto/Wire\.hpp[>\"]))"
)


def offending_lines(text: str) -> list[int]:
    """Returns the 1-based numbers of the lines of @p text that include a forbidden header."""
    return [number for number, line in enumerate(text.splitlines(), 1) if FORBIDDEN.match(line)]


def self_test() -> int:
    allowed = (
        "#include <vthost/keeper/Codec.hpp>",
        "#include <vthost/proto/Wire.hpp>",
        "#include <vtpty/ChildProcess.hpp>",
        "#include <core/codec/Fields.hpp>",
    )
    forbidden = (
        "#include <vtbackend/screen/Screen.hpp>",
        "#include <vtparser/Parser.hpp>",
        "#include <vtworkspace/Tab.hpp>",
        "#include <vthost/SessionHost.hpp>",
        "#include <vthost/proto/Frame.hpp>",
        '#  include "vthost/Daemon.hpp"',
    )
    failures = [line for line in allowed if offending_lines(line)]
    failures += [line for line in forbidden if not offending_lines(line)]
    for line in failures:
        print(f"self-test: wrong verdict for: {line}", file=sys.stderr)
    return 1 if failures else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--self-test", action="store_true")
    if parser.parse_args().self_test:
        return self_test()

    found = 0
    for path in sorted((REPO_ROOT / KEEPER_ROOT).rglob("*")):
        if path.suffix not in (".hpp", ".cpp", ".h"):
            continue
        for number in offending_lines(path.read_text(encoding="utf-8")):
            print(f"{path.relative_to(REPO_ROOT)}:{number}: the keeper must not include the engine")
            found += 1
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
