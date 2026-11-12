"""The families of compiler-cache entries CI saves to the GitHub Actions cache, named once.

Two scripts read this table: `prune-cache-generations.py`, which keeps the newest generation of each
key and deletes the rest, and `check-workflows.py`, which requires every job that saves such an entry
to be awaited by the job that prunes them. A new compiler cache is one more row here.
See docs/internals/build-times.md ("The compiler cache").
"""

from __future__ import annotations

import re
from dataclasses import dataclass


@dataclass(frozen=True)
class CacheFamily:
    """One kind of compiler-cache entry."""

    # What the family is, for messages.
    description: str
    # The action whose step saves an entry of this family.
    saver: str
    # What the step's `with.key` starts with, or "" when the action names the entry itself.
    key_prefix: str
    # A whole entry key: `name` is the key that one generation after another is saved under; the
    # rest is what tells the generations apart.
    generation: re.Pattern[str]


CCACHE = CacheFamily(
    description="ccache directory",
    # The action saves `ccache-<key>-<timestamp>` from whatever key the step names.
    saver="hendrikmuhs/ccache-action",
    key_prefix="",
    generation=re.compile(r"^(?P<name>ccache-.+)-\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z$"),
)

PACE_STORE = CacheFamily(
    description="Pace store file",
    # Saved by the Windows job as `pace-store-<job>-<pace version>-<run id>-<run attempt>`.
    saver="actions/cache/save",
    key_prefix="pace-store-",
    generation=re.compile(r"^(?P<name>pace-store-.+)-\d+-\d+$"),
)

FAMILIES: tuple[CacheFamily, ...] = (CCACHE, PACE_STORE)


def generation_name(key: str) -> str | None:
    """Returns the name `key` is a generation of, or None when it belongs to no family."""
    for family in FAMILIES:
        match = family.generation.match(key)
        if match is not None:
            return match["name"]
    return None


def saves_compiler_cache(step: dict[str, object]) -> bool:
    """Whether a workflow step (its parsed mapping) saves an entry of one of the families."""
    uses = str(step.get("uses", ""))
    with_ = step.get("with")
    key = str(with_.get("key", "")) if isinstance(with_, dict) else ""
    return any(
        uses.startswith(f"{family.saver}@") and key.startswith(family.key_prefix) for family in FAMILIES
    )
