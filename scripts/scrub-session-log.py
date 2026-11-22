#! /usr/bin/env python3
"""Scrubs a Claude Code session log into a fixture that holds its structure and nothing personal.

Run it with `python3 -I`. It reads one session log (`<config>/projects/<escaped-cwd>/<uuid>.jsonl`) recorded in
a scratch directory and writes the scrubbed log to standard output. The scrubbing is an ALLOWLIST: an entry
keeps only the keys named below for its type, a message block only its own, and everything else is dropped --
so a key a newer Claude Code adds never reaches a fixture unread.

- Every entry keeps `type`, `subtype`, `uuid`, `parentUuid`, `timestamp`, `version`, `cwd`, `sessionId`,
  `isMeta`, `isSidechain` and `isCompactSummary`; `user` and `assistant` also keep `message.role` and
  `message.content`.
- A content block keeps `type`, `name`, `id`, `tool_use_id` and `is_error`, plus the text of a `text` block,
  the content of a `tool_result` block and the input of a `tool_use` block -- the scratch task's own words.
  A `thinking` block keeps its type only.
- An `api-request*` entry becomes `{"type": ...}`.
- A kept payload of an unexpected shape (a `text` that is not a string, a `content` that is neither a
  string nor a list, an `input` that is not an object) is dropped.
- In every kept string, each UUID becomes a numbered placeholder `00000000-0000-4000-8000-NNNNNNNNNNNN`
  and each tool-use id `toolu_NNNNNNNNNNNNNNNNNNNNNNNN` (the same id, the same placeholder), the scratch project `--project` becomes `/home/user/proj` and the
  scratch root `--scratch` becomes `/home/user`.

`--self-test` scrubs a planted line and checks that nothing outside the allowlist survives.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from typing import Any

UUID = re.compile(r"[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}")

# An Anthropic tool-use id, which a tool_use block's `id` and its result's `tool_use_id` share.
TOOL_USE_ID = re.compile(r"toolu_[A-Za-z0-9]+")

ENTRY_KEYS = (
    "type",
    "subtype",
    "uuid",
    "parentUuid",
    "timestamp",
    "version",
    "cwd",
    "sessionId",
    "isMeta",
    "isSidechain",
    "isCompactSummary",
)

# The entry types whose message is kept; every other type keeps the entry keys only.
MESSAGE_TYPES = ("user", "assistant")

BLOCK_KEYS = ("type", "name", "id", "tool_use_id", "is_error")

# The one payload key a block type keeps beside BLOCK_KEYS, and the shapes it may have: any other shape is
# dropped, so that a newer Claude Code nesting something unread inside it never reaches a fixture.
BLOCK_PAYLOAD = {
    "text": ("text", (str,)),
    "tool_result": ("content", (str, list)),
    "tool_use": ("input", (dict,)),
}

PROJECT_PLACEHOLDER = "/home/user/proj"
SCRATCH_PLACEHOLDER = "/home/user"


class Scrubber:
    """Rewrites the kept strings of one log; the UUID numbering spans the whole log."""

    def __init__(self, project: list[str], scratch: list[str]) -> None:
        # Longest spelling first, so that a project path is never half-rewritten as the scratch root.
        self._paths = sorted(
            [(path, PROJECT_PLACEHOLDER) for path in project if path]
            + [(path, SCRATCH_PLACEHOLDER) for path in scratch if path],
            key=lambda row: len(row[0]),
            reverse=True,
        )
        self._uuids: dict[str, str] = {}
        self._toolUseIds: dict[str, str] = {}

    def _placeholder(self, match: re.Match[str]) -> str:
        key = match.group(0).lower()
        if key not in self._uuids:
            self._uuids[key] = f"00000000-0000-4000-8000-{len(self._uuids) + 1:012d}"
        return self._uuids[key]

    def _toolUseId(self, match: re.Match[str]) -> str:
        key = match.group(0)
        if key not in self._toolUseIds:
            self._toolUseIds[key] = f"toolu_{len(self._toolUseIds) + 1:024d}"
        return self._toolUseIds[key]

    def text(self, value: str) -> str:
        for path, placeholder in self._paths:
            value = value.replace(path, placeholder)
        return TOOL_USE_ID.sub(self._toolUseId, UUID.sub(self._placeholder, value))

    def value(self, value: Any) -> Any:
        """Rewrites every string inside @p value; keys of a kept object (tool input) are kept as they are."""
        if isinstance(value, str):
            return self.text(value)
        if isinstance(value, list):
            return [self.value(item) for item in value]
        if isinstance(value, dict):
            return {key: self.value(item) for key, item in value.items()}
        return value

    def block(self, block: Any) -> Any:
        if not isinstance(block, dict):
            return None
        kept = {key: self.value(block[key]) for key in BLOCK_KEYS if key in block}
        row = BLOCK_PAYLOAD.get(block.get("type"))
        if row is None:
            return kept
        payload, shapes = row
        content = block.get(payload)
        if not isinstance(content, shapes):
            return kept
        if isinstance(content, list):
            content = [self.block(inner) for inner in content]
        kept[payload] = self.value(content)
        return kept

    def entry(self, entry: Any) -> Any:
        if not isinstance(entry, dict):
            return {}
        kind = entry.get("type")
        if isinstance(kind, str) and kind.startswith("api-request"):
            return {"type": kind}
        kept = {key: self.value(entry[key]) for key in ENTRY_KEYS if key in entry}
        message = entry.get("message")
        if kind in MESSAGE_TYPES and isinstance(message, dict):
            scrubbed: dict[str, Any] = {}
            if "role" in message:
                scrubbed["role"] = self.value(message["role"])
            content = message.get("content")
            if isinstance(content, str):
                scrubbed["content"] = self.text(content)
            elif isinstance(content, list):
                scrubbed["content"] = [self.block(block) for block in content]
            kept["message"] = scrubbed
        return kept

    def line(self, line: str) -> str:
        return json.dumps(self.entry(json.loads(line)), ensure_ascii=False, separators=(",", ":"))


def self_test() -> int:
    scrubber = Scrubber(["/tmp/tmp.abc/proj"], ["/tmp/tmp.abc"])
    planted = {
        "type": "assistant",
        "uuid": "7da9cb6b-fec6-41fe-9c88-8a955b2975aa",
        "parentUuid": "4F4591CE-1044-42A9-AAE9-E05A268BCC0D",
        "sessionId": "7da9cb6b-fec6-41fe-9c88-8a955b2975aa",
        "cwd": "/tmp/tmp.abc/proj/sub",
        "gitBranch": "secret-branch",
        "requestId": "req_secret",
        "accountUuid": "someone@example.invalid",
        "message": {
            "role": "assistant",
            "model": "secret-model",
            "usage": {"input_tokens": 1},
            "content": [
                {"type": "thinking", "thinking": "secret thought", "signature": "secret-signature"},
                {"type": "text", "text": "wrote /tmp/tmp.abc/proj/one.txt", "citations": "secret"},
                {"type": "tool_use", "id": "toolu_01SecretAbc", "name": "Write", "caller": "secret",
                 "input": {"file_path": "/tmp/tmp.abc/proj/one.txt", "content": "one"}},
                {"type": "text", "text": {"secret": "an object where a string belongs"}},
                {"type": "tool_result", "tool_use_id": "toolu_01SecretAbc", "content": 7},
                {"type": "tool_result", "tool_use_id": "toolu_02SecretDef",
                 "content": [{"type": "text", "text": "kept"}, {"type": "image", "source": "secret"}]},
            ],
        },
    }
    blob = {"type": "api-request-blob", "hash": "secret", "message": {"role": "user", "content": "secret"}}
    scrubbed = scrubber.line(json.dumps(planted)) + "\n" + scrubber.line(json.dumps(blob))
    expected_first = {
        "type": "assistant",
        "uuid": "00000000-0000-4000-8000-000000000001",
        "parentUuid": "00000000-0000-4000-8000-000000000002",
        "cwd": "/home/user/proj/sub",
        "sessionId": "00000000-0000-4000-8000-000000000001",
        "message": {
            "role": "assistant",
            "content": [
                {"type": "thinking"},
                {"type": "text", "text": "wrote /home/user/proj/one.txt"},
                {"type": "tool_use", "name": "Write", "id": "toolu_000000000000000000000001",
                 "input": {"file_path": "/home/user/proj/one.txt", "content": "one"}},
                {"type": "text"},
                {"type": "tool_result", "tool_use_id": "toolu_000000000000000000000001"},
                {"type": "tool_result", "tool_use_id": "toolu_000000000000000000000002",
                 "content": [{"type": "text", "text": "kept"}, {"type": "image"}]},
            ],
        },
    }
    first, second = scrubbed.splitlines()
    failures = []
    if json.loads(first) != expected_first:
        failures.append(f"assistant entry scrubbed as {first}")
    if json.loads(second) != {"type": "api-request-blob"}:
        failures.append(f"api-request-blob scrubbed as {second}")
    for word in ("secret", "Secret", "@", "tmp.abc", "7da9cb6b"):
        if word in scrubbed:
            failures.append(f"{word!r} survived the scrubbing")
    for failure in failures:
        print(f"self-test: {failure}", file=sys.stderr)
    return 1 if failures else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--self-test", action="store_true", help="scrub a planted line and check the result")
    parser.add_argument("--project", action="append", default=[],
                        help="a spelling of the scratch project directory (repeatable)")
    parser.add_argument("--scratch", action="append", default=[],
                        help="a spelling of the scratch root (repeatable)")
    parser.add_argument("log", nargs="?", help="the session log to scrub")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if not args.log or not args.project:
        parser.error("a log and --project are required")
    scrubber = Scrubber(args.project, args.scratch)
    with open(args.log, encoding="utf-8") as log:
        for line in log:
            if line.strip():
                print(scrubber.line(line))
    return 0


if __name__ == "__main__":
    sys.exit(main())
