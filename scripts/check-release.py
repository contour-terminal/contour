#! /usr/bin/env python3
"""Checks that metainfo.xml describes a release, and that a release tag agrees with it.

    check-release.py [--tag NAME] [--metainfo PATH] [--self-test]

The newest release (the first <release> under <releases>) must carry a date, must not be of type
"development" and must have a major.minor.patch version. With --tag, the tag must be exactly
"v" + that version, must be annotated, and the release date must be the tag's date (taken in the
tagger's own timezone), so that a release build can be re-run from its tag days later. Without a tag
that exists yet -- the owner's check before tagging -- the release date must be today.

Exits 0 when the release passes and 1 otherwise; each problem goes to stderr as
"[RELEASE CHECK FAILED] <problem>". Python rather than shell so that it is self-tested by CTest and
runs on Windows too.
"""

from __future__ import annotations

import argparse
import datetime
import enum
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
VERSION_PATTERN = re.compile(r"\d+\.\d+\.\d+")
DEFAULT_METAINFO = REPO_ROOT / "metainfo.xml"


class TagKind(enum.Enum):
    """What a release tag name resolves to in the repository."""

    ABSENT = enum.auto()
    LIGHTWEIGHT = enum.auto()
    ANNOTATED = enum.auto()


@dataclass(frozen=True)
class Release:
    """The newest <release> of metainfo.xml."""

    version: str
    date: str | None
    type: str | None


@dataclass(frozen=True)
class Tag:
    """A tag as git reports it; `date` is the tagger date (YYYY-MM-DD) of an annotated tag."""

    name: str
    kind: TagKind
    date: str | None


def newest_release(metainfo_text: str) -> Release:
    """Returns the first <release> under <releases>; comments are dropped by the XML parser."""
    release = ET.fromstring(metainfo_text).find("./releases/release")
    if release is None:
        raise ValueError("metainfo.xml has no <release>")
    return Release(
        version=release.get("version", ""),
        date=release.get("date") or None,
        type=release.get("type") or None,
    )


def release_problems(release: Release, tag: Tag | None, today: str) -> list[str]:
    """Returns every reason `release` (and `tag`, when given) is not releasable."""
    problems: list[str] = []
    if not VERSION_PATTERN.fullmatch(release.version):
        problems.append(f"release version {release.version} is not major.minor.patch")
    if release.date is None:
        problems.append("release date must be present")
    if release.type == "development":
        problems.append("release type must not be 'development'")

    # What the release date must equal, and how to name it; None when the tag cannot say.
    expected_date: tuple[str, str] | None = (today, f"today ({today})")
    if tag is not None:
        expected_name = f"v{release.version}"
        if tag.name != expected_name:
            problems.append(
                f"tag {tag.name} does not match metainfo.xml's version {release.version}: "
                f"expected {expected_name}"
            )
        if tag.kind is TagKind.LIGHTWEIGHT:
            problems.append(f"tag {tag.name} is lightweight; release tags must be annotated (git tag -a)")
            expected_date = None
        elif tag.kind is TagKind.ANNOTATED:
            if tag.date is None:
                problems.append(f"tag {tag.name} is annotated but has no tagger date")
                expected_date = None
            else:
                expected_date = (tag.date, f"the tag's date {tag.date}")
    if release.date is not None and expected_date is not None and release.date != expected_date[0]:
        problems.append(f"release date {release.date} is not {expected_date[1]}")
    return problems


def tag_placement_problems(tag_name: str, tag_commit: str, head: str) -> list[str]:
    """Returns the problem when an existing tag points at another commit than the one checked out."""
    if tag_commit == head:
        return []
    return [f"tag {tag_name} points at {tag_commit}, not at the checked-out commit {head}"]


def parse_tag(name: str, for_each_ref_output: str) -> Tag:
    """Reads `git for-each-ref --format='%(objecttype) %(taggerdate:short)'` output for one tag."""
    object_type, _, date = for_each_ref_output.strip().partition(" ")
    if not object_type:
        return Tag(name, TagKind.ABSENT, None)
    if object_type == "tag":
        return Tag(name, TagKind.ANNOTATED, date.strip() or None)
    return Tag(name, TagKind.LIGHTWEIGHT, None)


def _git(*arguments: str) -> str:
    return subprocess.run(
        ["git", *arguments], cwd=REPO_ROOT, check=True, capture_output=True, text=True
    ).stdout.strip()


def tag_from_git(name: str) -> Tag:
    """Asks git what `name` is: absent, a lightweight tag, or an annotated tag and its date."""
    return parse_tag(name, _git("for-each-ref", "--format=%(objecttype) %(taggerdate:short)", f"refs/tags/{name}"))


def _metainfo(*releases: str) -> str:
    return f"<component><releases>{''.join(releases)}</releases></component>"


_DATED = '<release version="0.9.0" date="2026-11-07"/>'

_CASES: list[tuple[str, str, Tag | None, str, list[str]]] = [
    (
        "re-run days after the tag",
        _metainfo(_DATED),
        Tag("v0.9.0", TagKind.ANNOTATED, "2026-11-07"),
        "2026-11-09",
        [],
    ),
    (
        "tag for another version",
        _metainfo(_DATED),
        Tag("v0.9.1", TagKind.ANNOTATED, "2026-11-07"),
        "2026-11-09",
        ["does not match metainfo.xml's version 0.9.0: expected v0.9.0"],
    ),
    (
        "old tag form with a run number",
        _metainfo(_DATED),
        Tag("v0.9.0.9500", TagKind.ANNOTATED, "2026-11-07"),
        "2026-11-09",
        ["does not match metainfo.xml's version 0.9.0: expected v0.9.0"],
    ),
    (
        "lightweight tag",
        _metainfo(_DATED),
        Tag("v0.9.0", TagKind.LIGHTWEIGHT, None),
        "2026-11-07",
        ["is lightweight; release tags must be annotated (git tag -a)"],
    ),
    (
        "tag dated another day",
        _metainfo(_DATED),
        Tag("v0.9.0", TagKind.ANNOTATED, "2026-11-08"),
        "2026-11-08",
        ["release date 2026-11-07 is not the tag's date 2026-11-08"],
    ),
    (
        "tag not created yet, release dated today",
        _metainfo(_DATED),
        Tag("v0.9.0", TagKind.ABSENT, None),
        "2026-11-07",
        [],
    ),
    (
        "tag not created yet, release dated another day",
        _metainfo(_DATED),
        Tag("v0.9.0", TagKind.ABSENT, None),
        "2026-11-08",
        ["release date 2026-11-07 is not today (2026-11-08)"],
    ),
    (
        "tag not created yet, other version",
        _metainfo(_DATED),
        Tag("v0.9.1", TagKind.ABSENT, None),
        "2026-11-07",
        ["does not match metainfo.xml's version 0.9.0: expected v0.9.0"],
    ),
    (
        "tag not created yet, old form with a run number",
        _metainfo(_DATED),
        Tag("v0.9.0.9500", TagKind.ABSENT, None),
        "2026-11-07",
        ["does not match metainfo.xml's version 0.9.0: expected v0.9.0"],
    ),
    (
        "tag not created yet, spelled as a ref",
        _metainfo(_DATED),
        Tag("refs/tags/v0.9.0", TagKind.ABSENT, None),
        "2026-11-07",
        ["expected v0.9.0"],
    ),
    (
        "annotated tag without a tagger date",
        _metainfo(_DATED),
        Tag("v0.9.0", TagKind.ANNOTATED, None),
        "2026-11-07",
        ["is annotated but has no tagger date"],
    ),
    ("no tag, release dated today", _metainfo(_DATED), None, "2026-11-07", []),
    (
        "development release without a date",
        _metainfo('<release version="0.9.0" type="development"/>'),
        None,
        "2026-11-07",
        ["release date must be present", "release type must not be 'development'"],
    ),
    (
        "commented-out template above the release",
        _metainfo('<!-- <release version="x.y.z" type="development"/> -->', _DATED),
        None,
        "2026-11-07",
        [],
    ),
    (
        "placeholder version",
        _metainfo('<release version="x.y.z" date="2026-11-07"/>'),
        None,
        "2026-11-07",
        ["is not major.minor.patch"],
    ),
]


_PARSE_CASES: list[tuple[str, str, Tag]] = [
    ("annotated tag", "tag 2026-11-07", Tag("v0.9.0", TagKind.ANNOTATED, "2026-11-07")),
    ("lightweight tag", "commit ", Tag("v0.9.0", TagKind.LIGHTWEIGHT, None)),
    ("absent tag", "", Tag("v0.9.0", TagKind.ABSENT, None)),
    ("annotated tag without a tagger date", "tag ", Tag("v0.9.0", TagKind.ANNOTATED, None)),
]


def self_test() -> int:
    """Runs every case against `release_problems` and `parse_tag`; returns the process exit code."""
    failed = False
    for description, output, expected_tag in _PARSE_CASES:
        if parse_tag("v0.9.0", output) != expected_tag:
            failed = True
            print(f"check-release self-test: FAILED: parse_tag: {description}", file=sys.stderr)
    placement = (tag_placement_problems("v0.9.0", "abc", "abc"), tag_placement_problems("v0.9.0", "abc", "def"))
    if placement[0] or len(placement[1]) != 1:
        failed = True
        print(f"check-release self-test: FAILED: tag_placement_problems: {placement}", file=sys.stderr)
    for description, metainfo, tag, today, expected in _CASES:
        problems = release_problems(newest_release(metainfo), tag, today)
        matched = len(problems) == len(expected) and all(
            any(want in problem for problem in problems) for want in expected
        )
        if not matched:
            failed = True
            print(
                f"check-release self-test: FAILED: {description}: expected {expected}, got {problems}",
                file=sys.stderr,
            )
    if failed:
        return 1
    print(f"check-release self-test: {len(_CASES) + len(_PARSE_CASES) + 2} cases OK.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--tag", help="the release tag to check against metainfo.xml, e.g. v0.9.0")
    parser.add_argument("--metainfo", type=Path, default=DEFAULT_METAINFO, help="path of metainfo.xml")
    parser.add_argument("--self-test", action="store_true", help="check the rules against inline samples")
    args = parser.parse_args()
    if args.self_test:
        return self_test()

    try:
        release = newest_release(args.metainfo.read_text(encoding="utf-8"))
    except (OSError, ValueError, ET.ParseError) as error:
        print(f"[RELEASE CHECK FAILED] cannot read {args.metainfo}: {error}", file=sys.stderr)
        return 1
    tag = tag_from_git(args.tag) if args.tag is not None else None
    problems = release_problems(release, tag, datetime.date.today().isoformat())
    if tag is not None and tag.kind is not TagKind.ABSENT and args.metainfo == DEFAULT_METAINFO:
        # The checked-out tree is what was read; the tag must name that very commit.
        problems += tag_placement_problems(
            tag.name, _git("rev-parse", f"refs/tags/{tag.name}^{{commit}}"), _git("rev-parse", "HEAD")
        )
    for problem in problems:
        print(f"[RELEASE CHECK FAILED] {problem}", file=sys.stderr)
    if problems:
        return 1
    print(f"release check: {release.version} OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
