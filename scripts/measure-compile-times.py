#! /usr/bin/env python3
"""Measures per-translation-unit compile time and header cost, reproducibly.

Runs every .cpp command of a build tree's compile_commands.json with `-ftime-trace`, bypassing any
compiler launcher, and aggregates the Clang trace files into DIR/report.md: total CPU seconds split
into frontend and backend, a per-module table, the slowest TUs, the headers with the highest summed
inclusive parse time, and the most expensive template instantiations.

    measure-compile-times.py --build-dir out/clang-release [--jobs N] [--out DIR] [--filter REGEX]
    measure-compile-times.py --aggregate-only TRACEDIR [--out DIR]
    measure-compile-times.py --self-test

Traces land in OUT/traces (one `<index>.json` per TU, plus an `index.json` that maps each to its source
file and wall time), so `--aggregate-only OUT/traces` re-reports without recompiling. Wall times taken
with several jobs in parallel include contention; compare runs only at the same --jobs.
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
import time
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
LAUNCHERS = frozenset({"ccache", "sccache", "pace-cc", "distcc"})
SOURCE_SUFFIXES = (".cpp", ".cc", ".cxx")
TRACE_GRANULARITY_US = 200
TOP_TUS = 25
TOP_HEADERS = 40
TOP_INSTANTIATIONS = 25
INSTANTIATION_EVENTS = frozenset({"InstantiateClass", "InstantiateFunction"})
INDEX_NAME = "index.json"


def parse_trace(events: list[dict]) -> dict:
    """Reduces one trace's events to phase totals, header times and instantiation times (microseconds)."""
    phases: dict[str, int] = defaultdict(int)
    headers: dict[str, int] = defaultdict(int)
    instantiations: dict[str, int] = defaultdict(int)
    open_sources: dict[object, tuple[str, int]] = {}

    for event in events:
        name = event.get("name", "")
        phase = event.get("ph")
        detail = (event.get("args") or {}).get("detail", "")
        if phase == "X":
            duration = int(event.get("dur", 0))
            if name in ("Total Frontend", "Total Backend"):
                phases[name] += duration
            elif name == "Source" and detail:
                headers[detail] += duration
            elif name in INSTANTIATION_EVENTS and detail:
                instantiations[detail] += duration
        elif phase == "b" and name == "Source":
            open_sources[event.get("id")] = (detail, int(event.get("ts", 0)))
        elif phase == "e" and name == "Source":
            begun = open_sources.pop(event.get("id"), None)
            if begun is not None and begun[0]:
                headers[begun[0]] += int(event.get("ts", 0)) - begun[1]

    return {"phases": dict(phases), "headers": dict(headers), "instantiations": dict(instantiations)}


def is_translation_unit(path: str) -> bool:
    return path.endswith(SOURCE_SUFFIXES)


def module_of(source: str) -> str:
    """Maps a source path to its module: src/<module>, vendor/<name>, or the build tree's generated code."""
    try:
        relative = Path(source).resolve().relative_to(REPO_ROOT)
    except ValueError:
        return "(build tree)"
    parts = relative.parts
    if len(parts) >= 2 and parts[0] in ("src", "vendor", "third_party"):
        return f"{parts[0]}/{parts[1]}"
    if parts and parts[0] == "out":
        return "(generated, in build tree)"
    return parts[0] if parts else "(other)"


def aggregate(records: list[dict]) -> dict:
    """Combines per-TU records ({source, wall, trace}) into report data."""
    frontend = backend = 0
    modules: dict[str, dict] = defaultdict(lambda: {"tus": 0, "wall": 0.0, "frontend": 0, "backend": 0})
    headers: dict[str, dict] = defaultdict(lambda: {"us": 0, "tus": 0})
    instantiations: dict[str, dict] = defaultdict(lambda: {"us": 0, "count": 0})
    units = []
    for record in records:
        trace = record["trace"]
        tu_frontend = trace["phases"].get("Total Frontend", 0)
        tu_backend = trace["phases"].get("Total Backend", 0)
        frontend += tu_frontend
        backend += tu_backend
        module = modules[module_of(record["source"])]
        module["tus"] += 1
        module["wall"] += record["wall"]
        module["frontend"] += tu_frontend
        module["backend"] += tu_backend
        units.append({"source": record["source"], "wall": record["wall"],
                      "frontend": tu_frontend, "backend": tu_backend})  # fmt: skip
        for header, duration in trace["headers"].items():
            if is_translation_unit(header):
                continue
            headers[header]["us"] += duration
            headers[header]["tus"] += 1
        for name, duration in trace["instantiations"].items():
            instantiations[name]["us"] += duration
            instantiations[name]["count"] += 1
    return {
        "tus": len(records),
        "wall": sum(record["wall"] for record in records),
        "frontend": frontend,
        "backend": backend,
        "modules": dict(modules),
        "headers": dict(headers),
        "instantiations": dict(instantiations),
        "units": units,
    }


def _seconds(microseconds: int) -> str:
    return f"{microseconds / 1e6:.1f}"


def _short(path: str) -> str:
    try:
        return str(Path(path).resolve().relative_to(REPO_ROOT))
    except ValueError:
        return path


def _cell(text: str) -> str:
    return text.replace("|", "\\|")


def render_report(data: dict) -> str:
    lines = ["# Compile-time report", ""]
    lines += [f"- Translation units: {data['tus']}",
              f"- Total wall CPU-s (sum over TUs): {data['wall']:.1f}",
              f"- Frontend CPU-s: {_seconds(data['frontend'])}",
              f"- Backend CPU-s: {_seconds(data['backend'])}", ""]  # fmt: skip

    lines += ["## Per module", "",
              "| Module | TUs | Wall CPU-s | Frontend s | Backend s |", "|---|---:|---:|---:|---:|"]  # fmt: skip
    for name, module in sorted(data["modules"].items(), key=lambda item: -item[1]["wall"]):
        lines.append(f"| {name} | {module['tus']} | {module['wall']:.1f} | "
                     f"{_seconds(module['frontend'])} | {_seconds(module['backend'])} |")  # fmt: skip

    lines += ["", f"## Top {TOP_TUS} translation units", "",
              "| TU | Wall s | Frontend s | Backend s |", "|---|---:|---:|---:|"]  # fmt: skip
    for unit in sorted(data["units"], key=lambda item: -item["wall"])[:TOP_TUS]:
        lines.append(f"| {_cell(_short(unit['source']))} | {unit['wall']:.1f} | "
                     f"{_seconds(unit['frontend'])} | {_seconds(unit['backend'])} |")  # fmt: skip

    lines += ["", f"## Top {TOP_HEADERS} headers by summed inclusive parse time", "",
              "| Header | Total s | #TUs | ms/TU |", "|---|---:|---:|---:|"]  # fmt: skip
    for header, entry in sorted(data["headers"].items(), key=lambda item: -item[1]["us"])[:TOP_HEADERS]:
        lines.append(f"| {_cell(_short(header))} | {_seconds(entry['us'])} | {entry['tus']} | "
                     f"{entry['us'] / entry['tus'] / 1e3:.1f} |")  # fmt: skip

    lines += ["", f"## Top {TOP_INSTANTIATIONS} template instantiations", "",
              "| Instantiation | Total s | #TUs |", "|---|---:|---:|"]  # fmt: skip
    for name, entry in sorted(data["instantiations"].items(), key=lambda item: -item[1]["us"])[:TOP_INSTANTIATIONS]:
        lines.append(f"| {_cell(name[:110])} | {_seconds(entry['us'])} | {entry['count']} |")
    return "\n".join(lines) + "\n"


def command_words(entry: dict) -> list[str]:
    """The entry's compiler command as an argument vector, without a leading launcher."""
    words = list(entry["arguments"]) if "arguments" in entry else shlex.split(entry["command"])
    while words and Path(words[0]).name in LAUNCHERS:
        words.pop(0)
    return words


def trace_command(words: list[str], output: Path) -> list[str]:
    """Rewrites the command to emit a time trace and an object file at `output`."""
    result = []
    skip_next = False
    for word in words:
        if skip_next:
            skip_next = False
            continue
        if word == "-o":
            skip_next = True
            continue
        if word.startswith("-o") and len(word) > 2 or word.startswith("-ftime-trace"):
            continue
        result.append(word)
    return result + [f"-ftime-trace={output}.json", f"-ftime-trace-granularity={TRACE_GRANULARITY_US}",
                     "-o", str(output)]  # fmt: skip


def compile_one(index: int, entry: dict, trace_dir: Path) -> dict:
    with tempfile.TemporaryDirectory(prefix="mct-") as scratch:
        output = Path(scratch) / "out.o"
        words = trace_command(command_words(entry), output)
        started = time.monotonic()
        completed = subprocess.run(words, cwd=entry["directory"], capture_output=True, text=True)
        wall = time.monotonic() - started
        if completed.returncode != 0:
            raise RuntimeError(f"compiling {entry['file']} failed:\n{completed.stderr}")
        trace_file = Path(f"{output}.json")
        if not trace_file.exists():
            raise RuntimeError(f"no time trace produced for {entry['file']}")
        (trace_dir / f"{index}.json").write_bytes(trace_file.read_bytes())
    return {"index": index, "source": str(Path(entry["directory"]) / entry["file"]), "wall": wall,
            "directory": entry["directory"]}  # fmt: skip


def run_build(build_dir: Path, jobs: int, trace_dir: Path, pattern: str | None) -> None:
    entries = json.loads((build_dir / "compile_commands.json").read_text())
    selected = [entry for entry in entries if is_translation_unit(entry["file"])]
    if pattern:
        regex = re.compile(pattern)
        selected = [entry for entry in selected if regex.search(entry["file"])]
    if not selected:
        raise SystemExit("no translation units selected")
    trace_dir.mkdir(parents=True, exist_ok=True)
    print(f"measuring {len(selected)} translation units with {jobs} jobs", file=sys.stderr)
    records = []
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = [pool.submit(compile_one, index, entry, trace_dir) for index, entry in enumerate(selected)]
        for done, future in enumerate(futures, start=1):
            records.append(future.result())  # raises on the first failed TU, with its stderr
            if done % 100 == 0:
                print(f"  {done}/{len(selected)}", file=sys.stderr)
    (trace_dir / INDEX_NAME).write_text(json.dumps(records))


def merge_spellings(headers: dict[str, int], directory: str) -> dict[str, int]:
    """Keys headers by their normalized absolute path, so spellings of one file are summed."""
    merged: dict[str, int] = defaultdict(int)
    for header, duration in headers.items():
        merged[os.path.normpath(os.path.join(directory, header))] += duration
    return dict(merged)


def load_records(trace_dir: Path) -> list[dict]:
    index = json.loads((trace_dir / INDEX_NAME).read_text())
    records = []
    for item in index:
        events = json.loads((trace_dir / f"{item['index']}.json").read_text()).get("traceEvents", [])
        trace = parse_trace(events)
        directory = item.get("directory", "")
        # The same header is spelled differently across TUs (relative to the build directory, or with `..`).
        trace["headers"] = merge_spellings(trace["headers"], directory)
        records.append({"source": item["source"], "wall": item["wall"], "trace": trace})
    return records


def self_test() -> int:
    failures = []

    def check(condition: bool, description: str) -> None:
        if not condition:
            failures.append(description)

    def source(ph: str, **extra) -> dict:
        return {"name": "Source", "ph": ph, "pid": 1, "tid": 1, **extra}

    complete = [  # ph "X" shape
        source("X", ts=0, dur=1000, args={"detail": "/inc/a.hpp"}),
        source("X", ts=0, dur=5000, args={"detail": "/src/x/one.cpp"}),
        {"name": "Total Frontend", "ph": "X", "ts": 0, "dur": 4000, "pid": 1, "tid": 1},
        {"name": "Total Backend", "ph": "X", "ts": 0, "dur": 900, "pid": 1, "tid": 1},
        {"name": "InstantiateClass", "ph": "X", "ts": 0, "dur": 300, "args": {"detail": "std::vector<int>"}},
    ]
    paired = [  # ph "b"/"e" shape
        source("b", ts=100, id="0x1", args={"detail": "/inc/a.hpp"}),
        source("e", ts=2100, id="0x1"),
        source("b", ts=0, id="0x2", args={"detail": "/inc/b.hpp"}),
        source("e", ts=50, id="0x2"),
        {"name": "Total Frontend", "ph": "X", "ts": 0, "dur": 6000, "pid": 1, "tid": 1},
        {"name": "InstantiateClass", "ph": "X", "ts": 0, "dur": 100, "args": {"detail": "std::vector<int>"}},
    ]
    data = aggregate([
        {"source": "/src/x/one.cpp", "wall": 1.0, "trace": parse_trace(complete)},
        {"source": "/src/x/two.cpp", "wall": 2.0, "trace": parse_trace(paired)},
    ])  # fmt: skip
    check(data["tus"] == 2, "two TUs")
    check(data["headers"]["/inc/a.hpp"] == {"us": 3000, "tus": 2}, f"a.hpp 2 TUs, 3000us: {data['headers']}")
    check("/src/x/one.cpp" not in data["headers"], "TU sources are not reported as headers")
    check(data["headers"]["/inc/b.hpp"] == {"us": 50, "tus": 1}, "b.hpp from a b/e pair")
    check(data["frontend"] == 10000, f"Total Frontend summed: {data['frontend']}")
    check(data["backend"] == 900, f"Total Backend summed: {data['backend']}")
    check(data["instantiations"]["std::vector<int>"] == {"us": 400, "count": 2}, "instantiations summed")
    report = render_report(data)
    check("a.hpp" in report and "| 2 |" in report, "report lists a.hpp with its TU count")

    words = command_words({"command": "ccache /usr/bin/clang++ -DX=\\\"abc\\\" -c f.cpp -o f.o"})
    check(words == ["/usr/bin/clang++", '-DX="abc"', "-c", "f.cpp", "-o", "f.o"],
          f"launcher stripped, quoting preserved: {words}")  # fmt: skip
    rewritten = trace_command(words, Path("/t/o.o"))
    check("f.o" not in rewritten and rewritten[-2:] == ["-o", "/t/o.o"], f"output rewritten: {rewritten}")

    if failures:
        for failure in failures:
            print(f"measure-compile-times self-test: FAILED: {failure}", file=sys.stderr)
        return 1
    print("measure-compile-times self-test: OK.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--self-test", action="store_true", help="check the aggregator on embedded traces")
    parser.add_argument("--build-dir", type=Path, help="build tree holding compile_commands.json")
    parser.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2))
    parser.add_argument("--out", type=Path, default=Path("compile-times"), help="output directory")
    parser.add_argument("--filter", help="only measure sources whose path matches this regex")
    parser.add_argument("--aggregate-only", type=Path, metavar="TRACEDIR", help="re-report existing traces")
    args = parser.parse_args()

    if args.self_test:
        return self_test()
    if args.aggregate_only:
        trace_dir = args.aggregate_only
    elif args.build_dir:
        trace_dir = args.out / "traces"
        run_build(args.build_dir, args.jobs, trace_dir, args.filter)
    else:
        parser.error("one of --build-dir, --aggregate-only or --self-test is required")
    args.out.mkdir(parents=True, exist_ok=True)
    report = render_report(aggregate(load_records(trace_dir)))
    (args.out / "report.md").write_text(report)
    print(f"wrote {args.out / 'report.md'}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
