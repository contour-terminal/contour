#! /usr/bin/env python3
"""Checks that a production build reached Contour's own code and its binary, and nothing else.

CONTOUR_PRODUCTION_BUILD turns on whole-program optimization and the platforms' hardening flags for
Contour's own targets. A flag can silently fail to arrive (a target created before the option was read,
a toolchain that ignores it) or leak into a dependency, and the binary is the only place the effect is
certain. This gate reads two things the build leaves behind:

* `compile_commands.json`: every first-party translation unit carries each flag in the manifest's
  `compile_flags`; no third-party translation unit carries one.
* the linked binary: the PE load-config / ELF dynamic section shows the hardening is really there, found
  by one probe command per format. A Mach-O binary has no row (its only ones were about debug symbols,
  which no build publishes any more), so on macOS only the compile database is checked.

The check proves the compile flags and the binary's properties; it does not prove that a link-only flag
(/OPT:REF, /OPT:ICF, -Wl,--as-needed, -Wl,-dead_strip) reached the linker, because link options are not in the
compile database and no binary row depends on them.

`--self-test` checks both rules against inline fixtures and needs no build tree. Python rather than
shell, so that ctest runs it on every platform.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

SOURCE_SUFFIXES = (".c", ".cc", ".cpp", ".cxx", ".mm")

# (path prefix, flag, reason): a flag a dependency's own CMake sets. The prefix is relative to the source or
# the binary directory, like the manifest's third_party_dirs. A row is added only with a reason.
EXEMPT: list[tuple[str, str, str]] = []

# format -> (description, regex) requirements against the probe output.
BINARY_REQUIREMENTS: dict[str, list[tuple[str, str]]] = {
    "pe": [
        ("CF instrumented", r"CF instrumented"),
        ("FID table present", r"FID table present"),
        ("CET compatible", r"CET compatible"),
        ("Dynamic base", r"Dynamic base"),
        ("NX compatible", r"NX compatible"),
        ("High Entropy Virtual Addresses", r"High Entropy Virtual Addresses"),
    ],
    "elf": [
        ("Type DYN", r"Type:\s+DYN"),
        ("PIE (FLAGS_1)", r"\(FLAGS_1\).*\bPIE\b"),
        ("GNU_RELRO", r"GNU_RELRO"),
        ("BIND_NOW", r"\(BIND_NOW\)|\(FLAGS\).*\bBIND_NOW\b|\(FLAGS_1\).*\bNOW\b"),
        ("__stack_chk_fail", r"__stack_chk_fail"),
    ],
    # No rows: every Mach-O row was about debug symbols, which are no longer produced. The compile
    # database checks cover macOS.
    "macho": [],
}

def normalize(path: str) -> str:
    """Forward slashes, case-folded on Windows, so manifest and compile database paths compare."""
    path = path.replace("\\", "/")
    return path.casefold() if sys.platform == "win32" else path


def is_under(path: str, directory: str) -> bool:
    return path == directory or path.startswith(directory.rstrip("/") + "/")


def command_tokens(entry: dict) -> list[str]:
    arguments = entry.get("arguments")
    return list(arguments) if arguments else entry.get("command", "").split()


def entry_path(entry: dict) -> str:
    file = entry["file"].replace("\\", "/")
    if not (file.startswith("/") or re.match(r"^[A-Za-z]:/", file)):
        file = entry.get("directory", "").replace("\\", "/").rstrip("/") + "/" + file
    return normalize(file)


def is_under_roots(path: str, directories: list[str], manifest: dict) -> bool:
    """Whether a normalized path lies under one of @p directories, relative to the source or binary directory."""
    roots = [normalize(manifest["source_dir"]), normalize(manifest["binary_dir"])]
    return any(is_under(path, f"{root.rstrip('/')}/{normalize(d)}") for root in roots for d in directories)


def classify(path: str, manifest: dict) -> str | None:
    """Returns "first", "third" or None for a translation unit path (already normalized)."""
    if is_under_roots(path, manifest["first_party_dirs"], manifest):
        return "first"
    if is_under_roots(path, manifest["third_party_dirs"], manifest):
        return "third"
    return None


def compile_command_problems(entries: list[dict], manifest: dict) -> list[str]:
    """Returns one message per flag rule the compile database breaks, empty when it is clean."""
    return check_compile_commands(entries, manifest)[0]


def check_compile_commands(entries: list[dict], manifest: dict) -> tuple[list[str], int, int]:
    """Returns the problems of the compile database and its first-party and third-party translation unit counts."""
    wanted = manifest["compile_flags"]
    first = third = 0
    # (what is wrong, flag) -> [translation unit count, first offender]: a whole tree missing one flag
    # is one finding, not one line per translation unit.
    findings: dict[tuple[str, str], list] = {}

    def note(what: str, flag: str, file: str) -> None:
        findings.setdefault((what, flag), [0, file])[0] += 1

    for entry in entries:
        if not entry["file"].endswith(SOURCE_SUFFIXES):
            continue
        path = entry_path(entry)
        kind = classify(path, manifest)
        if kind is None:
            continue
        tokens = set(command_tokens(entry))
        if kind == "first":
            first += 1
            for flag in wanted:
                if flag not in tokens:
                    note("first-party translation units lack", flag, entry["file"])
        else:
            third += 1
            exempt = {flag for prefix, flag, _ in EXEMPT if is_under_roots(path, [prefix], manifest)}
            for flag in wanted:
                if flag in tokens and flag not in exempt:
                    note("third-party translation units carry", flag, entry["file"])
    problems = [f"{count} {what} {flag} (first: {file})" for (what, flag), (count, file) in findings.items()]
    if first == 0:
        problems.append("no first-party translation unit in the compile database; the check would pass vacuously")
    if third == 0:
        problems.append("no third-party translation unit in the compile database; the check would pass vacuously")
    return problems, first, third


def binary_problems(fmt: str, outputs: dict[str, str]) -> list[str]:
    """Returns one message per hardening requirement the binary misses.

    `outputs` holds the probe output under the key "binary". A format without rows (macho) has no problems.
    """
    if fmt not in BINARY_REQUIREMENTS:
        return [f"unknown binary format {fmt!r}"]
    text = outputs.get("binary", "")
    return [f"{fmt} binary lacks {what}" for what, pattern in BINARY_REQUIREMENTS[fmt]
            if not re.search(pattern, text)]


def self_test() -> int:
    manifest = {
        "format": "pe", "source_dir": "/w/contour", "binary_dir": "/w/contour/out/b",
        "first_party_dirs": ["src", "examples"], "third_party_dirs": ["_deps", "vendor"],
        "compile_flags": ["/GL", "/guard:cf"], "tool": "/x/dumpbin",
    }

    def tu(file: str, *flags: str) -> dict:
        return {"directory": "/w/contour/out/b", "file": file, "command": "cl " + " ".join(flags) + " /c " + file}

    own = tu("/w/contour/src/a.cpp", "/GL", "/guard:cf")
    dep = tu("/w/contour/out/b/_deps/x/b.cpp", "/O2")
    compile_cases = {
        "all flags, clean dependency": ([own, dep], 0),
        "first-party without /GL": ([tu("/w/contour/src/a.cpp", "/guard:cf"), dep], 1),
        "third-party with /guard:cf": ([own, tu("/w/contour/vendor/c.cpp", "/guard:cf")], 1),
        "no third-party entry": ([own], 1),
        "generated moc source": ([tu("/w/contour/out/b/src/contour/x_autogen/mocs_compilation.cpp", "/GL", "/guard:cf"),
                                  dep], 0),
        "outside both lists": ([own, dep, tu("/elsewhere/z.cpp")], 0),
        "resource file skipped": ([own, dep, tu("/w/contour/src/a.rc")], 0),
    }
    pe = ("CF instrumented\nFID table present\nCET compatible\nDynamic base\nNX compatible\n"
          "High Entropy Virtual Addresses\n")
    elf = ("Type:  DYN (Position-Independent Executable file)\n"
           " 0x000000000000001e (FLAGS)  BIND_NOW\n 0x000000006ffffffb (FLAGS_1) Flags: PIE\n"
           " GNU_RELRO 0x0\n 1: UND __stack_chk_fail\n [28] .text PROGBITS\n")
    binary_cases = {
        "pe complete, without a PDB reference": ("pe", {"binary": pe}, 0),
        "pe without CET": ("pe", {"binary": pe.replace("CET compatible\n", "")}, 1),
        "pe without CFG": ("pe", {"binary": pe.replace("CF instrumented\n", "")}, 1),
        "elf complete, without .debug_info": ("elf", {"binary": elf}, 0),
        "elf FLAGS_1 without PIE": ("elf", {"binary": elf.replace("Flags: PIE", "Flags:")}, 1),
        "elf without RELRO": ("elf", {"binary": elf.replace("GNU_RELRO", "GNU_STACK")}, 1),
        "macho has no binary rows": ("macho", {}, 0),
        "unknown format": ("coff", {}, 1),
    }
    failed = []
    for name, (entries, expected) in compile_cases.items():
        got = compile_command_problems(entries, manifest)
        if len(got) != expected:
            failed.append((name, got))
    for name, (fmt, outputs, expected) in binary_cases.items():
        got = binary_problems(fmt, outputs)
        if len(got) != expected:
            failed.append((name, got))
    # Every row of the binary tables must be able to fail alone: drop the lines that evidence one row.
    for fmt, text in (("pe", pe), ("elf", elf)):
        for what, pattern in BINARY_REQUIREMENTS[fmt]:
            broken = "\n".join(line for line in text.splitlines() if not re.search(pattern, line))
            got = binary_problems(fmt, {"binary": broken})
            if got != [f"{fmt} binary lacks {what}"]:
                failed.append((f"{fmt} row {what}", got))
    own_only = compile_command_problems([own], manifest)
    if len(own_only) != 1 or "third-party" not in own_only[0]:
        failed.append(("no third-party translation unit", own_only))
    dep_only = compile_command_problems([dep], manifest)
    if len(dep_only) != 1 or "first-party" not in dep_only[0]:
        failed.append(("no first-party translation unit", dep_only))
    vendored = tu("/w/contour/vendor/c.cpp", "/guard:cf")
    if not compile_command_problems([own, vendored], manifest):
        failed.append(("a third-party flag is not reported without an exemption", []))
    EXEMPT.append(("vendor", "/guard:cf", "self-test: a dependency's own CMake sets it"))
    try:
        exempted = compile_command_problems([own, vendored], manifest)
    finally:
        EXEMPT.pop()
    if exempted:
        failed.append(("exempt flag in a third-party translation unit", exempted))
    for name, got in failed:
        print(f"check-production-build self-test: FAILED: {name}: {got}", file=sys.stderr)
    if failed:
        return 1
    print("check-production-build self-test: OK.")
    return 0


def run_probe(command: list[str]) -> str:
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise RuntimeError(f"{' '.join(command)} exited {result.returncode}: {result.stderr.strip()}")
    return result.stdout


def check_binary(manifest: dict, binary: Path) -> list[str]:
    fmt = manifest["format"]
    if fmt == "pe":
        return binary_problems(fmt, {"binary": run_probe([manifest["tool"], "/headers", "/loadconfig", str(binary)])})
    if fmt == "elf":
        probe = [manifest["tool"], "-h", "-l", "-d", "-S", "-W", "--dyn-syms", str(binary)]
        return binary_problems(fmt, {"binary": run_probe(probe)})
    return binary_problems(fmt, {})


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--self-test", action="store_true", help="check the rules against inline fixtures")
    parser.add_argument("--manifest", type=Path, help="JSON manifest written by the build")
    parser.add_argument("--binary", type=Path, help="the linked main binary")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if args.manifest is None or args.binary is None:
        parser.error("--manifest and --binary are required without --self-test")
    try:
        manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
        database = Path(manifest["binary_dir"]) / "compile_commands.json"
        entries = json.loads(database.read_text(encoding="utf-8"))
        problems, first, third = check_compile_commands(entries, manifest)
        problems += check_binary(manifest, args.binary)
    except (RuntimeError, OSError, ValueError, KeyError) as error:
        print(f"check-production-build: {type(error).__name__}: {error}", file=sys.stderr)
        return 1
    for problem in problems:
        print(f"check-production-build: {problem}", file=sys.stderr)
    if problems:
        return 1
    print(f"check-production-build: OK ({first} first-party, {third} third-party translation units)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
