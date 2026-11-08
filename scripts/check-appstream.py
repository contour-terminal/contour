#! /usr/bin/env python3
"""Checks metainfo.xml and the application .desktop files against what Flathub's build accepts.

    check-appstream.py
    check-appstream.py --self-test

Three layers, so a defect fails here rather than on Flathub's CI after a release is tagged:

1. Flathub's rules that neither validator below enforces, checked in Python on every host:
   description markup is the strict subset AppStream renders (<p>, <ul>/<ol> of <li>, and only <em>
   and <code> inline -- no <kbd>, no <strong>); no Markdown backticks, which reach the store page as
   literal characters; screenshot captions of at most 100 characters; and every release dated except
   the in-progress one at the top, which `type="development"` marks and the release bump stamps.
2. `appstreamcli validate --no-net` on metainfo.xml. The in-progress release is given today's date
   in a scratch copy first, so the validator judges everything else; line numbers are kept.
3. `desktop-file-validate` on every Type=Application entry under src/contour/packaging. The
   Type=Service files there are KDE Dolphin service menus, a format of KDE's own that the XDG
   validator does not know and rejects wholesale, so they are left out by their Type.

A validator that is not installed is skipped with a note, so `ctest -L lint` stays green on a host
without it. CI sets CHECK_APPSTREAM_REQUIRE_TOOL=1, which turns that skip into a failure.

`--self-test` checks layer 1 and the scratch-copy dating against inline samples and needs no tool.
Python rather than shell, so ctest runs it on every platform.
"""

from __future__ import annotations

import argparse
import datetime
import re
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ElementTree
from dataclasses import dataclass
from pathlib import Path

from lib.toolcache import REPO_ROOT, missing_tool

METAINFO = REPO_ROOT / "metainfo.xml"
DESKTOP_DIR = REPO_ROOT / "src" / "contour" / "packaging"
REQUIRE_ENV = "CHECK_APPSTREAM_REQUIRE_TOOL"

# Flathub's limit on a screenshot caption, in characters.
MAX_CAPTION_LENGTH = 100

# The markup AppStream renders in a <description>: each element, and the children it may hold.
ALLOWED_CHILDREN: dict[str, frozenset[str]] = {
    "description": frozenset({"p", "ul", "ol"}),
    "p": frozenset({"em", "code"}),
    "ul": frozenset({"li"}),
    "ol": frozenset({"li"}),
    "li": frozenset({"em", "code"}),
    "em": frozenset(),
    "code": frozenset(),
}

# The elements whose text a store page shows; a backtick in any of them is shown literally.
SHOWN_TEXT = ("name", "summary", "caption", "description")

COMMENT_RE = re.compile(r"<!--.*?-->", re.DOTALL)


@dataclass(frozen=True)
class Validator:
    """An external validator: the program, its fixed arguments, and how it reports a problem."""

    program: str
    arguments: tuple[str, ...]
    # desktop-file-validate exits 0 on a hint or a deprecation it still prints; anything it says is
    # something Flathub's own run of it would show, so its output alone fails the check.
    output_is_failure: bool = False


APPSTREAMCLI = Validator("appstreamcli", ("validate", "--no-net", "--explain"))
DESKTOP_FILE_VALIDATE = Validator("desktop-file-validate", (), output_is_failure=True)


def markup_problems(description: ElementTree.Element, where: str) -> list[str]:
    """Returns one message per element in `description` that AppStream's markup subset does not allow."""
    problems = []
    for parent in description.iter():
        allowed = ALLOWED_CHILDREN.get(parent.tag, frozenset())
        problems += [
            f"{where}: <{child.tag}> is not allowed inside <{parent.tag}>"
            for child in parent
            if child.tag not in allowed
        ]
    return problems


def problems_in(text: str) -> list[str]:
    """Returns one message per Flathub rule the metainfo text breaks, empty when it keeps them all."""
    try:
        root = ElementTree.fromstring(text)
    except ElementTree.ParseError as error:
        return [f"not well-formed XML: {error}"]

    problems = []
    for description in root.iter("description"):
        problems += markup_problems(description, "description")

    for tag in SHOWN_TEXT:
        for element in root.iter(tag):
            shown = "".join(element.itertext())
            if "`" in shown:
                snippet = shown.strip()[:60]
                problems.append(f"<{tag}> holds a backtick, shown literally; use <code>: {snippet!r}")

    for caption in root.iter("caption"):
        length = len("".join(caption.itertext()).strip())
        if length > MAX_CAPTION_LENGTH:
            problems.append(f"<caption> is {length} characters, over Flathub's {MAX_CAPTION_LENGTH}")

    for index, release in enumerate(root.iter("release")):
        dated = release.get("date") or release.get("timestamp")
        in_progress = index == 0 and release.get("type") == "development"
        if not (dated or in_progress):
            problems.append(
                f'<release version="{release.get("version")}"> has no date; only the in-progress '
                'release at the top (type="development") may go without one'
            )
    return problems


def dated_for_validation(text: str, today: str) -> str:
    """Returns `text` with the in-progress release dated `today`, so a validator judges the rest.

    Only the first <release> outside a comment is touched, and only when it is type="development"
    and undated; every other release is the validator's to judge as it stands. The attribute is
    inserted on the tag's own line, so the validator's line numbers still point into metainfo.xml.
    Text, not a parse, for that reason; the tag ends at its first `>`, which no attribute of a
    <release> holds.
    """
    blanked = COMMENT_RE.sub(lambda match: re.sub(r"[^\n]", " ", match.group(0)), text)
    match = re.search(r"<release\b", blanked)
    if match is None:
        return text
    end = blanked.index(">", match.end())
    tag = blanked[match.start() : end]
    if 'type="development"' not in tag or re.search(r"\s(date|timestamp)=", tag):
        return text
    return f'{text[: match.end()]} date="{today}"{text[match.end() :]}'


def application_desktop_files(directory: Path) -> list[Path]:
    """Returns the Type=Application desktop entries in `directory`, sorted by name."""
    return sorted(
        path
        for path in directory.glob("*.desktop")
        if re.search(r"^Type\s*=\s*Application\s*$", path.read_text(encoding="utf-8"), re.MULTILINE)
    )


def run_validator(validator: Validator, files: list[Path]) -> int:
    """Runs `validator` on each of `files` and returns the failures; a missing validator is one, or a skip."""
    program = shutil.which(validator.program)
    if program is None:
        return missing_tool(validator.program, REQUIRE_ENV, "not installed")
    failures = 0
    for path in files:
        result = subprocess.run([program, *validator.arguments, str(path)], capture_output=True, text=True)
        output = (result.stdout + result.stderr).strip()
        if result.returncode != 0 or (validator.output_is_failure and output):
            failures += 1
            print(f"{validator.program} {path.name}:\n{output}", file=sys.stderr)
    return failures


def self_test() -> int:
    def metainfo(body: str, releases: str = '<release version="1.0" date="2026-01-01"/>') -> str:
        return f"<component>{body}<releases>{releases}</releases></component>"

    caption_100 = "x" * MAX_CAPTION_LENGTH
    # Each sample breaks at most one rule; the second field is a phrase of the one problem it must
    # report, or None for a sample that must pass -- so a sample cannot pass on another rule's account.
    cases = {
        "allowed markup": (metainfo("<description><p>a <code>b</code> <em>c</em></p>"
                                    "<ul><li><code>d</code></li></ul></description>"), None),
        "kbd": (metainfo("<description><p><kbd>Ctrl</kbd></p></description>"), "<kbd> is not allowed inside <p>"),
        "strong in a list item": (metainfo("<description><ul><li><strong>x</strong></li></ul></description>"),
                                  "<strong> is not allowed inside <li>"),
        "nested list": (metainfo("<description><ul><li><ul><li>x</li></ul></li></ul></description>"),
                        "<ul> is not allowed inside <li>"),
        "text straight in a list": (metainfo("<description><ul><p>x</p></ul></description>"),
                                    "<p> is not allowed inside <ul>"),
        "backtick in a release": (metainfo("", '<release version="1.0" date="2026-01-01">'
                                               "<description><p>run `x`</p></description></release>"),
                                  "<description> holds a backtick"),
        "backtick in the summary": (metainfo("<summary>a `b`</summary>"), "<summary> holds a backtick"),
        "caption at the limit": (metainfo(f"<caption>{caption_100}</caption>"), None),
        "caption over the limit": (metainfo(f"<caption>{caption_100}y</caption>"), "is 101 characters"),
        "in-progress release undated": (metainfo("", '<release version="2.0" type="development"/>'
                                                     '<release version="1.0" date="2026-01-01"/>'), None),
        "stable top release undated": (metainfo("", '<release version="2.0"/>'), '"2.0"> has no date'),
        "older release undated": (metainfo("", '<release version="2.0" type="development"/>'
                                               '<release version="1.0" type="development"/>'), '"1.0"> has no date'),
        "malformed": ("<component>", "not well-formed"),
    }

    def reports(text: str, expected: str | None) -> bool:
        problems = problems_in(text)
        return not problems if expected is None else len(problems) == 1 and expected in problems[0]

    failed = [name for name, (text, expected) in cases.items() if not reports(text, expected)]

    # (input, expected output); an expected None means the text comes back unchanged.
    dating = {
        "skips a commented-out release": (
            '<!--\n<release version="x" type="development">\n-->\n<release version="2" type="development">',
            '<!--\n<release version="x" type="development">\n-->\n<release date="D" version="2" type="development">',
        ),
        "leaves a dated release": ('<release version="2" type="development" date="E">', None),
        "leaves a stable release": ('<release version="2">', None),
        "reads date= as an attribute only": ('<release version="2" x-date="E" type="development">',
                                             '<release date="D" version="2" x-date="E" type="development">'),
        "leaves only the first": ('<release version="2" type="development"><release version="1" type="development">',
                                  '<release date="D" version="2" type="development"><release version="1" type="development">'),
    }
    failed += [
        name
        for name, (text, expected) in dating.items()
        if dated_for_validation(text, "D") != (text if expected is None else expected)
    ]

    with tempfile.TemporaryDirectory() as scratch:
        directory = Path(scratch)
        (directory / "app.desktop").write_text("[Desktop Entry]\nType=Application\nName=A\n", encoding="utf-8")
        (directory / "menu.desktop").write_text("[Desktop Entry]\nType=Service\n", encoding="utf-8")
        if [path.name for path in application_desktop_files(directory)] != ["app.desktop"]:
            failed.append("selects only Type=Application entries")

    for name in failed:
        print(f"check-appstream self-test: FAILED: {name}", file=sys.stderr)
    if failed:
        return 1
    print(f"check-appstream self-test: {len(cases) + len(dating) + 1} cases OK.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--self-test", action="store_true", help="check the rules against inline samples")
    if parser.parse_args().self_test:
        return self_test()

    text = METAINFO.read_text(encoding="utf-8")
    failures = 0
    for problem in problems_in(text):
        print(f"{METAINFO.relative_to(REPO_ROOT).as_posix()}: {problem}", file=sys.stderr)
        failures += 1

    with tempfile.TemporaryDirectory() as scratch:
        # Named as installed, which is what appstreamcli expects of a metainfo file.
        dated = Path(scratch) / "org.contourterminal.Contour.metainfo.xml"
        dated.write_text(dated_for_validation(text, datetime.date.today().isoformat()), encoding="utf-8")
        failures += run_validator(APPSTREAMCLI, [dated])
        failures += run_validator(DESKTOP_FILE_VALIDATE, application_desktop_files(DESKTOP_DIR))

    if failures:
        return 1
    print("check-appstream: OK.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
