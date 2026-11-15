#! /usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Scales a recorded `gh-prs` reply up to the largest one the command can print.

`gh-prs` asks for at most 100 pull requests (`--limit 100`), so its largest reply holds 100. Recording one
needs an author with 100 open pull requests; this builds it from a smaller recording instead: the recorded
rows are repeated in order until there are COUNT of them, each copy given a number and a head branch of
its own, so that every row stays a distinct pull request. The input and output are the command's
positional projection (its `--jq` filter, core/Commands.cpp): one array per pull request, number first,
then title, URL and head branch. The output is written the way `gh --jq` writes it (compact, non-ASCII
kept, one trailing newline), so its size is the size `gh` would print, and it is deterministic: the same
input always yields the same bytes.

Usage: scale-fixture.py [--count N] <recorded.json> <scaled.json>
    e.g. scale-fixture.py core/testdata/github/prs-open.json core/testdata/github/prs-100.json
"""

from __future__ import annotations

import argparse
import copy
import json
import sys
from pathlib import Path

# The number of pull requests `gh-prs` asks for: its `--limit` (core/Commands.cpp).
DEFAULT_COUNT = 100

# Where a projected row holds what each copy changes.
NUMBER = 0
URL = 2
HEAD = 3

# The first number a scaled copy gets; above every recorded one, so that no copy collides with an original.
FIRST_NUMBER = 100000


def scaled(recorded: list[list], count: int) -> list[list]:
    """`count` rows: `recorded` repeated in order, each copy numbered and branched on its own."""
    rows = []
    for index in range(count):
        row = copy.deepcopy(recorded[index % len(recorded)])
        number = FIRST_NUMBER + index
        row[NUMBER] = number
        row[HEAD] = f"{row[HEAD]}-{index}"
        row[URL] = row[URL].rsplit("/", 1)[0] + f"/{number}"
        rows.append(row)
    return rows


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--count", type=int, default=DEFAULT_COUNT, help="how many pull requests to write")
    parser.add_argument("recorded", type=Path, help="a recorded `gh-prs` reply")
    parser.add_argument("output", type=Path, help="where to write the scaled reply")
    arguments = parser.parse_args()

    recorded = json.loads(arguments.recorded.read_text(encoding="utf-8"))
    if not isinstance(recorded, list) or not recorded:
        print(f"{arguments.recorded}: not a non-empty JSON array", file=sys.stderr)
        return 1
    text = json.dumps(scaled(recorded, arguments.count), separators=(",", ":"), ensure_ascii=False) + "\n"
    arguments.output.write_text(text, encoding="utf-8", newline="\n")
    size = len(text.encode("utf-8"))
    print(f"{arguments.output}: {arguments.count} pull requests, {size} bytes ({size / 1024:.1f} KiB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
