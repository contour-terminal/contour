#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check that every Tracy macro used under src/ is defined by the no-op stub header.

Call sites use Tracy's macros unconditionally, and the stub at
src/crispy/tracy-stub/tracy/Tracy.hpp is what makes them resolve when the build is configured with
CONTOUR_TRACY=OFF -- the default, and what CI and every packaging build use. A macro used in src/
but missing from the stub therefore breaks only the default build, and only for whoever next
compiles that file. This gate turns that into an immediate, named failure.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
STUB = REPO_ROOT / "src" / "crispy" / "tracy-stub" / "tracy" / "Tracy.hpp"
SOURCE_ROOT = REPO_ROOT / "src"
SOURCE_SUFFIXES = (".cpp", ".hpp", ".h")

# Tracy's public macro vocabulary, as name shapes. Anything in an instrumented source file that
# looks like one of these must be defined by the stub.
TRACY_MACRO_PATTERN = re.compile(
    r"\b(?:Zone[A-Z][A-Za-z]*|Frame[A-Z][A-Za-z]*|Tracy[A-Z][A-Za-z]*"
    r"|LockableBase|SharedLockable[A-Za-z]*)\b"
)
DEFINE_PATTERN = re.compile(r"^\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)")


def stub_definitions() -> set[str]:
    """Collect the macro names the stub header defines.

    :return: The set of defined macro names.
    """
    return {
        match.group(1)
        for line in STUB.read_text(encoding="utf-8").splitlines()
        if (match := DEFINE_PATTERN.match(line))
    }


def instrumented_sources() -> list[Path]:
    """Collect the source files that include Tracy, excluding the stub itself.

    :return: The matching paths, sorted for stable output.
    """
    found = []
    for path in sorted(SOURCE_ROOT.rglob("*")):
        if path.suffix not in SOURCE_SUFFIXES or STUB.parent in path.parents:
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        if "tracy/Tracy.hpp" in text:
            found.append(path)
    return found


def main() -> int:
    if not STUB.is_file():
        print(f"error: stub header not found: {STUB}", file=sys.stderr)
        return 1

    defined = stub_definitions()
    missing: dict[str, list[str]] = {}
    scanned = instrumented_sources()

    for path in scanned:
        relative = path.relative_to(REPO_ROOT)
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
            stripped = line.lstrip()
            if stripped.startswith(("//", "#include", "*", "/*")):
                continue
            for name in TRACY_MACRO_PATTERN.findall(line):
                if name not in defined:
                    missing.setdefault(name, []).append(f"{relative}:{number}")

    if missing:
        print("error: Tracy macros used under src/ but not defined by the no-op stub:",
              file=sys.stderr)
        for name, sites in sorted(missing.items()):
            print(f"  {name}", file=sys.stderr)
            for site in sites[:5]:
                print(f"    {site}", file=sys.stderr)
        print(f"\nDefine them in {STUB.relative_to(REPO_ROOT)}.", file=sys.stderr)
        return 1

    print(f"check-tracy-stub: OK ({len(defined)} macros defined, "
          f"{len(scanned)} instrumented file(s) covered)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
