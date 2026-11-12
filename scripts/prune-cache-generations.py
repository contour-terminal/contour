#! /usr/bin/env python3
"""Deletes the superseded generations of the compiler caches a branch has saved.

`hendrikmuhs/ccache-action` saves a new entry per run, named `ccache-<key>-<timestamp>`, and never
deletes one. Two generations of every key do not fit GitHub's cache quota, and a pull request that
restores the older one keeps it "recently used", so GitHub evicts some other key instead -- which is
how a job ends up with no cache at all. This keeps the newest entry of each key and deletes the rest.

Run by `.github/workflows/cache-prune.yml` after each `Build` of master; see
docs/internals/build-times.md ("The compiler cache").
"""

from __future__ import annotations

import argparse
import enum
import json
import re
import subprocess
import sys
from collections.abc import Callable
from dataclasses import dataclass

# A compiler-cache entry: the action's `ccache-` prefix, the job's key, and the timestamp the action
# appends. Anything else in the cache store (vcpkg, Qt, FreeBSD, ...) does not match and is never touched.
_GENERATION = re.compile(r"^(?P<name>ccache-.+)-\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z$")


@dataclass(frozen=True)
class CacheEntry:
    """One entry of a repository's Actions cache."""

    id: int
    key: str
    ref: str
    created_at: str
    size_in_bytes: int = 0


def generations_to_delete(entries: list[CacheEntry], ref: str) -> list[int]:
    """Returns the ids of the entries on `ref` that a newer entry of the same key supersedes, ascending."""
    newest: dict[str, CacheEntry] = {}
    superseded: list[int] = []
    for entry in entries:
        match = _GENERATION.match(entry.key)
        if entry.ref != ref or match is None:
            continue
        name = match["name"]
        kept = newest.get(name)
        if kept is None:
            newest[name] = entry
        elif (entry.created_at, entry.id) > (kept.created_at, kept.id):
            newest[name] = entry
            superseded.append(kept.id)
        else:
            superseded.append(entry.id)
    return sorted(superseded)


class DeleteOutcome(enum.Enum):
    """What deleting one entry came to."""

    DELETED = "deleted"
    # GitHub evicted it between the listing and the delete: the entry is gone, which is what was wanted.
    ALREADY_GONE = "already gone"
    FAILED = "failed"


def prune(ids: list[int], delete: Callable[[int], DeleteOutcome]) -> list[int]:
    """Deletes each id through `delete`; returns the ids whose deletion failed."""
    return [entry_id for entry_id in ids if delete(entry_id) is DeleteOutcome.FAILED]


def list_entries(repo: str, ref: str) -> list[CacheEntry]:
    """Returns every cache entry `repo` holds for `ref`, through the `gh` CLI."""
    listing = subprocess.run(
        ["gh", "api", "--paginate", "--slurp", f"repos/{repo}/actions/caches?per_page=100&ref={ref}"],
        check=False,
        capture_output=True,
        text=True,
    )
    if listing.returncode != 0:
        raise RuntimeError(f"listing the caches of {repo} failed: {listing.stderr.strip()}")
    return [
        CacheEntry(cache["id"], cache["key"], cache["ref"], cache["created_at"], cache["size_in_bytes"])
        for page in json.loads(listing.stdout)
        for cache in page["actions_caches"]
    ]


def outcome_of_delete(exit_status: int, stderr: str) -> DeleteOutcome:
    """Classifies how a `gh api --method DELETE` on one cache entry ended."""
    if exit_status == 0:
        return DeleteOutcome.DELETED
    return DeleteOutcome.ALREADY_GONE if "HTTP 404" in stderr else DeleteOutcome.FAILED


def delete_entry(repo: str, entry_id: int) -> DeleteOutcome:
    """Deletes one cache entry of `repo` through the `gh` CLI."""
    deletion = subprocess.run(
        ["gh", "api", "--method", "DELETE", f"repos/{repo}/actions/caches/{entry_id}"],
        check=False,
        capture_output=True,
        text=True,
    )
    outcome = outcome_of_delete(deletion.returncode, deletion.stderr)
    if outcome is DeleteOutcome.FAILED:
        print(f"prune-cache-generations: deleting {entry_id} failed: {deletion.stderr.strip()}", file=sys.stderr)
    return outcome


_MASTER = "refs/heads/master"
_T1 = "2026-11-11T11:15:14.983Z"
_T2 = "2026-11-11T15:34:44.248Z"
_T3 = "2026-11-11T19:02:01.500Z"
_WINDOWS = "ccache-ccache-windows-msvc-release"
_X86 = "ccache-ccache-ubuntu-24.04-Clang 22-23-Release-6"
_ARM = "ccache-ccache-ubuntu-24.04-arm-Clang 22-23-Release-6"


def _generation(entry_id: int, name: str, stamp: str, ref: str = _MASTER) -> CacheEntry:
    return CacheEntry(entry_id, f"{name}-{stamp}", ref, stamp)


def self_test() -> int:
    cases: tuple[tuple[str, list[CacheEntry], list[int]], ...] = (
        ("two generations", [_generation(1, _WINDOWS, _T1), _generation(2, _WINDOWS, _T2)], [1]),
        (
            "three generations",
            [_generation(2, _WINDOWS, _T2), _generation(3, _WINDOWS, _T3), _generation(1, _WINDOWS, _T1)],
            [1, 2],
        ),
        ("single generation", [_generation(1, _WINDOWS, _T1)], []),
        ("prefix keys stay apart", [_generation(1, _X86, _T1), _generation(2, _ARM, _T2)], []),
        (
            "other refs untouched",
            [_generation(1, _WINDOWS, _T1, "refs/pull/2292/merge"), _generation(2, _WINDOWS, _T2)],
            [],
        ),
        (
            "non-ccache keys untouched",
            [
                CacheEntry(1, "vcpkg-Windows-842ce606", _MASTER, _T1),
                CacheEntry(2, "vcpkg-Windows-842ce606", _MASTER, _T2),
                CacheEntry(3, "freebsd-15.1-2.2.9-amd64-v3", _MASTER, _T1),
                CacheEntry(4, "freebsd-15.1-2.2.9-amd64-v3", _MASTER, _T2),
            ],
            [],
        ),
        (
            "no timestamp, untouched",
            [CacheEntry(1, "ccache-something", _MASTER, _T1), CacheEntry(2, "ccache-something", _MASTER, _T2)],
            [],
        ),
        ("tie", [_generation(4, _WINDOWS, _T1), _generation(9, _WINDOWS, _T1)], [4]),
    )
    failures: list[str] = []
    for name, entries, expected in cases:
        actual = generations_to_delete(entries, _MASTER)
        if actual != expected:
            failures.append(f"{name}: expected {expected}, got {actual}")

    outcomes = {1: DeleteOutcome.DELETED, 2: DeleteOutcome.ALREADY_GONE, 3: DeleteOutcome.FAILED}
    failed = prune([1, 2, 3], lambda entry_id: outcomes[entry_id])
    if failed != [3]:
        failures.append(f"already-gone delete is success: expected [3] to fail, got {failed}")

    # What `gh api` reports for each way a DELETE can end (exit status, stderr).
    gh_results = (
        (0, "", DeleteOutcome.DELETED),
        (1, "gh: Not Found (HTTP 404)", DeleteOutcome.ALREADY_GONE),
        (1, "gh: Resource not accessible by integration (HTTP 403)", DeleteOutcome.FAILED),
        (1, "gh: Server Error (HTTP 502)", DeleteOutcome.FAILED),
    )
    for exit_status, stderr, expected_outcome in gh_results:
        actual_outcome = outcome_of_delete(exit_status, stderr)
        if actual_outcome is not expected_outcome:
            failures.append(f"gh exit {exit_status} {stderr!r}: expected {expected_outcome}, got {actual_outcome}")

    for failure in failures:
        print(f"prune-cache-generations self-test: FAILED: {failure}", file=sys.stderr)
    if failures:
        return 1
    print(f"prune-cache-generations self-test: {len(cases) + 1 + len(gh_results)} cases OK.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--self-test", action="store_true", help="check the rule against inline fixtures")
    parser.add_argument("--repo", help="the repository, as OWNER/NAME")
    parser.add_argument("--ref", help="the ref whose caches are pruned, e.g. refs/heads/master")
    parser.add_argument("--dry-run", action="store_true", help="name what would be deleted; delete nothing")
    arguments = parser.parse_args()
    if arguments.self_test:
        return self_test()
    if not arguments.repo or not arguments.ref:
        parser.error("--repo and --ref are required")

    try:
        entries = {entry.id: entry for entry in list_entries(arguments.repo, arguments.ref)}
    except RuntimeError as error:
        print(f"prune-cache-generations: {error}", file=sys.stderr)
        return 1
    superseded = generations_to_delete(list(entries.values()), arguments.ref)
    verb = "would delete" if arguments.dry_run else "deleting"
    for entry_id in superseded:
        entry = entries[entry_id]
        print(f"prune-cache-generations: {verb} {entry.key} ({entry.size_in_bytes // (1024 * 1024)} MB)")
    if arguments.dry_run:
        return 0
    failed = prune(superseded, lambda entry_id: delete_entry(arguments.repo, entry_id))
    print(f"prune-cache-generations: {len(superseded) - len(failed)} of {len(superseded)} superseded entries removed.")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
