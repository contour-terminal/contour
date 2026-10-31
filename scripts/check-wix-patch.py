#! /usr/bin/env python3
"""Checks the WiX patch file CPack merges into the Windows installer, before a Windows runner has to.

CPack's WiX generator inserts each `<CPackWiXFragment Id="X">` into the element of the generated
sources whose Id is X, and refuses a patch file that names the same Id twice ("Invalid reuse of
'CPackWixFragment' 'Id'"): everything one Id contributes goes in one fragment. That error only shows
at `cpack` time, on Windows, after a full build, so this gate checks the rule on every platform.

`--self-test` checks the rule against inline samples and needs no tree. Python rather than shell, so
that ctest runs it on every platform.
"""

from __future__ import annotations

import argparse
import sys
import xml.etree.ElementTree as ElementTree
from collections import Counter
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
PATCH_FILE = REPO_ROOT / "src" / "contour" / "packaging" / "wix_patch.xml"


def problems_in(text: str) -> list[str]:
    """Returns one message per rule the patch file text breaks, empty when it is well-formed."""
    try:
        root = ElementTree.fromstring(text)
    except ElementTree.ParseError as error:
        return [f"not well-formed XML: {error}"]
    if root.tag != "CPackWiXPatch":
        return [f"root element is <{root.tag}>, expected <CPackWiXPatch>"]
    ids = Counter(fragment.get("Id") for fragment in root.iter("CPackWiXFragment"))
    problems = ["a <CPackWiXFragment> has no Id" for _ in range(ids.pop(None, 0))]
    problems += [
        f'CPackWiXFragment Id "{fragment_id}" is used {count} times; CPack accepts each Id once, '
        "so merge its fragments into one"
        for fragment_id, count in sorted(ids.items())
        if count > 1
    ]
    return problems


def self_test() -> int:
    cases = {
        "distinct ids": ('<CPackWiXPatch><CPackWiXFragment Id="#PRODUCT"/>'
                         '<CPackWiXFragment Id="#PRODUCTFEATURE"/></CPackWiXPatch>', 0),
        "reused id": ('<CPackWiXPatch><CPackWiXFragment Id="#PRODUCT"><a/></CPackWiXFragment>'
                      '<CPackWiXFragment Id="#PRODUCT"><b/></CPackWiXFragment></CPackWiXPatch>', 1),
        "missing id": ("<CPackWiXPatch><CPackWiXFragment/></CPackWiXPatch>", 1),
        "wrong root": ('<Patch><CPackWiXFragment Id="#PRODUCT"/></Patch>', 1),
        "malformed": ("<CPackWiXPatch>", 1),
    }
    failed = [name for name, (text, expected) in cases.items() if len(problems_in(text)) != expected]
    for name in failed:
        print(f"check-wix-patch self-test: FAILED: {name}: {problems_in(cases[name][0])}", file=sys.stderr)
    if failed:
        return 1
    print("check-wix-patch self-test: OK.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--self-test", action="store_true", help="check the rule against inline samples")
    if parser.parse_args().self_test:
        return self_test()
    problems = problems_in(PATCH_FILE.read_text(encoding="utf-8"))
    for problem in problems:
        print(f"{PATCH_FILE.relative_to(REPO_ROOT).as_posix()}: {problem}", file=sys.stderr)
    if problems:
        return 1
    print("check-wix-patch: OK.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
