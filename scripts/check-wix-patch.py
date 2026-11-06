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
PACKAGING_DIR = REPO_ROOT / "src" / "contour" / "packaging"
PATCH_FILE = PACKAGING_DIR / "wix_patch.xml"
# The WiX source each installer scope adds (CPACK_CONTOUR_WIX_<scope>_SOURCE in src/contour/CMakeLists.txt).
SCOPE_FILES = {"perMachine": PACKAGING_DIR / "wix_machine.wxs", "perUser": PACKAGING_DIR / "wix_user.wxs"}
WIX_NAMESPACE_URI = "http://wixtoolset.org/schemas/v4/wxs"


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


def local_name(tag: str) -> str:
    """Returns an element tag without its namespace."""
    return tag.rpartition("}")[2]


def scope_ids(root: ElementTree.Element) -> set[tuple[str, str]]:
    """Returns the (kind, Id) of what a scope file defines and the patch and the other scope rely on."""
    ids: set[tuple[str, str]] = set()
    for element in root.iter():
        kind = local_name(element.tag)
        if kind in ("Component", "Property", "Feature", "CustomAction"):
            identifier = element.get("Id")
        elif kind == "SetProperty":
            identifier = element.get("Action") or element.get("Id")
        else:
            continue
        if identifier is not None:
            ids.add((kind, identifier))
    return ids


def scope_problems(scope_texts: dict[str, str], patch_text: str) -> list[str]:
    """Returns one message per way the scope files disagree with each other or with the patch file."""
    roots: dict[str, ElementTree.Element] = {}
    problems: list[str] = []
    for scope, text in scope_texts.items():
        try:
            root = ElementTree.fromstring(text)
        except ElementTree.ParseError as error:
            problems.append(f"{scope}: not well-formed XML: {error}")
            continue
        if root.tag != f"{{{WIX_NAMESPACE_URI}}}Wix":
            problems.append(f"{scope}: root element is {root.tag}, expected {{{WIX_NAMESPACE_URI}}}Wix")
            continue
        roots[scope] = root
    defined = {scope: scope_ids(root) for scope, root in roots.items()}
    for scope, ids in defined.items():
        for other, other_ids in defined.items():
            problems += [
                f"{identifier} is defined for {other} but not for {scope}"
                for _, identifier in sorted(other_ids - ids)
            ]
    try:
        patch_root = ElementTree.fromstring(patch_text)
    except ElementTree.ParseError as error:
        return problems + [f"patch file is not well-formed XML: {error}"]
    for reference in patch_root.iter("ComponentRef"):
        identifier = reference.get("Id")
        lacking = [scope for scope, ids in defined.items() if ("Component", identifier) not in ids]
        if lacking:
            problems.append(
                f"the patch file refers to component {identifier}, which {', '.join(lacking)} does not define"
            )
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

    def scope(*component_ids: str) -> str:
        components = "".join(f'<Component Id="{component_id}"/>' for component_id in component_ids)
        return f'<Wix xmlns="{WIX_NAMESPACE_URI}"><Fragment>{components}</Fragment></Wix>'

    def patch(*references: str) -> str:
        refs = "".join(f'<ComponentRef Id="{reference}"/>' for reference in references)
        return f'<CPackWiXPatch><CPackWiXFragment Id="#PRODUCTFEATURE">{refs}</CPackWiXFragment></CPackWiXPatch>'

    scope_cases = {
        "scopes agree": (
            {"perMachine": scope("A", "B"), "perUser": scope("A", "B")}, patch("A"), 0),
        "a component one scope lacks": (
            {"perMachine": scope("A", "B"), "perUser": scope("A")}, patch("A"), 1),
        "a patch reference no scope defines": (
            {"perMachine": scope("A"), "perUser": scope("A")}, patch("A", "C"), 1),
    }
    for name, (texts, patch_text, expected) in scope_cases.items():
        found = scope_problems(texts, patch_text)
        if len(found) != expected:
            failed.append(name)
            print(f"check-wix-patch self-test: FAILED: {name}: {found}", file=sys.stderr)
    if failed:
        return 1
    print("check-wix-patch self-test: OK.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--self-test", action="store_true", help="check the rule against inline samples")
    if parser.parse_args().self_test:
        return self_test()
    patch_text = PATCH_FILE.read_text(encoding="utf-8")
    problems = problems_in(patch_text)
    problems += scope_problems(
        {scope: path.read_text(encoding="utf-8") for scope, path in SCOPE_FILES.items()}, patch_text
    )
    for problem in problems:
        print(f"{PATCH_FILE.relative_to(REPO_ROOT).as_posix()}: {problem}", file=sys.stderr)
    if problems:
        return 1
    print("check-wix-patch: OK.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
