#! /usr/bin/env python3
"""Renders the TSV records of `bench-latency record FILE` as Markdown tables for a step summary.

A record holds three kinds of line: a summary row (`metric mode count p50 p90 p99 max`, microseconds),
a scalar row (`metric mode count value`), and a `gate:` verdict line. Anything else is skipped.

Usage: latency-summary.py TITLE=FILE...
"""

from __future__ import annotations

import sys
from pathlib import Path

SUMMARY_HEADER = ("metric", "mode", "count", "p50 us", "p90 us", "p99 us", "max us")
SCALAR_HEADER = ("metric", "mode", "count", "median")
SUMMARY_COLUMNS = len(SUMMARY_HEADER)
SCALAR_COLUMNS = len(SCALAR_HEADER)


def markdown_table(header: tuple[str, ...], rows: list[list[str]]) -> list[str]:
    """Renders @p rows under @p header as a Markdown table."""
    lines = ["| " + " | ".join(header) + " |", "| " + " | ".join("---" for _ in header) + " |"]
    lines.extend("| " + " | ".join(row) + " |" for row in rows)
    return lines


def render(title: str, text: str) -> list[str]:
    """Renders one record under a heading named @p title."""
    summaries: list[list[str]] = []
    scalars: list[list[str]] = []
    gates: list[str] = []
    for line in text.splitlines():
        if line.startswith("gate:"):
            gates.append(line)
            continue
        fields = line.split("\t")
        if len(fields) == SUMMARY_COLUMNS:
            summaries.append(fields)
        elif len(fields) == SCALAR_COLUMNS:
            scalars.append(fields)
    out = [f"### {title}", ""]
    if summaries:
        out += markdown_table(SUMMARY_HEADER, summaries) + [""]
    if scalars:
        out += markdown_table(SCALAR_HEADER, scalars) + [""]
    out += [f"`{gate}`" for gate in gates]
    if not (summaries or scalars or gates):
        out += ["No measurements were recorded."]
    return out + [""]


def main(arguments: list[str]) -> int:
    """Prints the summary of every TITLE=FILE argument; a missing file is reported, not fatal."""
    lines: list[str] = []
    for argument in arguments:
        title, _, name = argument.partition("=")
        path = Path(name)
        if not path.is_file():
            lines += [f"### {title}", "", f"`{name}` was not written: the run failed before it recorded.", ""]
            continue
        lines += render(title, path.read_text(encoding="utf-8"))
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
