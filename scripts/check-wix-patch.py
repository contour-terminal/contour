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
# The WiX source the installer adds for every scope (CPACK_CONTOUR_WIX_SOURCE in src/contour/CMakeLists.txt).
SCOPE_FILE = PACKAGING_DIR / "wix_scope.wxs"
WIX_NAMESPACE_URI = "http://wixtoolset.org/schemas/v4/wxs"
# What wix_patch.xml's INSTALL_ROOT actions rely on the scope source to define, as (element, Id).
REQUIRED_SCOPE_IDS = (("SetProperty", "ContourRootDefault"), ("Property", "CONTOUR_RECORDED_ROOT"))


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
    """Returns the (kind, Id) of what a scope source defines and the patch file relies on."""
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


def scope_problems(scope_text: str, patch_text: str) -> list[str]:
    """Returns one message per way the scope source lacks what the patch file relies on.

    The source is one file for every install scope, which the WiX preprocessor branches (<?if?>, processing
    instructions the XML parser skips), so what it defines does not depend on the scope.
    """
    try:
        root = ElementTree.fromstring(scope_text)
    except ElementTree.ParseError as error:
        return [f"scope source is not well-formed XML: {error}"]
    if root.tag != f"{{{WIX_NAMESPACE_URI}}}Wix":
        return [f"scope source: root element is {root.tag}, expected {{{WIX_NAMESPACE_URI}}}Wix"]
    defined = scope_ids(root)
    problems = [
        f"the scope source does not define the {kind} {identifier} that wix_patch.xml relies on"
        for kind, identifier in REQUIRED_SCOPE_IDS
        if (kind, identifier) not in defined
    ]
    try:
        patch_root = ElementTree.fromstring(patch_text)
    except ElementTree.ParseError as error:
        return problems + [f"patch file is not well-formed XML: {error}"]
    problems += [
        f"the patch file refers to component {reference.get('Id')}, which the scope source does not define"
        for reference in patch_root.iter("ComponentRef")
        if ("Component", reference.get("Id")) not in defined
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

    def scope(*component_ids: str) -> str:
        components = "".join(f'<Component Id="{component_id}"/>' for component_id in component_ids)
        components += '<SetProperty Id="INSTALL_ROOT" Action="ContourRootDefault"/>'
        components += '<Property Id="CONTOUR_RECORDED_ROOT"/>'
        return f'<Wix xmlns="{WIX_NAMESPACE_URI}"><Fragment>{components}</Fragment></Wix>'

    def patch(*references: str) -> str:
        refs = "".join(f'<ComponentRef Id="{reference}"/>' for reference in references)
        return f'<CPackWiXPatch><CPackWiXFragment Id="#PRODUCTFEATURE">{refs}</CPackWiXFragment></CPackWiXPatch>'

    scope_cases = {
        "a scope source with what the patch relies on": (scope("A", "B"), patch("A"), 0),
        "a scope source lacking what the patch relies on": (
            f'<Wix xmlns="{WIX_NAMESPACE_URI}"><Component Id="A"/></Wix>', patch("A"), 2),
        "a patch reference the scope source does not define": (scope("A"), patch("A", "C"), 1),
        "a scope source branching on the scope": (
            scope("A").replace("<Fragment>", "<?if $(var.ContourScope) = perUser ?><Fragment>")
            .replace("</Fragment>", "</Fragment><?endif?>"), patch("A"), 0),
    }
    for name, (scope_text, patch_text, expected) in scope_cases.items():
        found = scope_problems(scope_text, patch_text)
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
    problems += scope_problems(SCOPE_FILE.read_text(encoding="utf-8"), patch_text)
    for problem in problems:
        print(f"{PATCH_FILE.relative_to(REPO_ROOT).as_posix()}: {problem}", file=sys.stderr)
    if problems:
        return 1
    print("check-wix-patch: OK.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
