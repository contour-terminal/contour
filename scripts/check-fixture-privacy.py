#! /usr/bin/env python3
r"""Keeps recorded fixtures free of anything that names a person, a machine or a credential.

Fixtures recorded from real tools -- a Claude Code session log, a forge CLI's JSON -- are scrubbed before they
are committed (scripts/scrub-session-log.py); this gate checks that the scrubbing held. Every file beneath the
fixture trees below is read, and a line fails on:

- any `@` (an e-mail address, a mention, an account);
- a UUID other than the scrubber's placeholders `00000000-0000-4000-8000-NNNNNNNNNNNN`, or 32 hexadecimal
  digits in a row (a UUID written without its dashes);
- a tool-use id other than the scrubber's placeholders `toolu_NNNNNNNNNNNNNNNNNNNNNNNN`;
- a home directory but the scrubbed `/home/user`: `/home/<name>`, `/Users/<name>`, `C:\Users\<name>` in
  either slash form, and the forms Claude Code escapes into a project directory name (`-home-<name>-`);
- a token or key: `glpat-`, `ghp_`, `gho_`, `ghs_`, `ghu_`, `ghr_`, `github_pat_`, `ATATT`, `xoxb-`, `xoxp-`,
  `sk-ant-`, an AWS key id (`AKIA` and 16 more), a PEM private key, and `bearer ` in any case.

Every offending line is printed as file:line: reason. `--self-test` holds every rule to an offending and a
clean sample and needs no tree. Python rather than shell, so that ctest runs it on every platform.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# The trees holding recorded (or recorded-shaped) fixtures. A tree that does not exist yet is skipped.
FIXTURE_TREES = (
    Path("src/contour/cli/testdata/agent-transcript"),
    Path("extensions/orchestrator/contract/agent-transcript"),
    Path("extensions/orchestrator/core/testdata/gitlab"),
    Path("extensions/orchestrator/core/testdata/jira"),
)

UUID = re.compile(r"[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}")
PLACEHOLDER_UUID = re.compile(r"00000000-0000-4000-8000-[0-9]{12}")
TOOL_USE_ID = re.compile(r"toolu_[A-Za-z0-9]*")
PLACEHOLDER_TOOL_USE_ID = re.compile(r"toolu_[0-9]{24}")

# A name that is not the scrubbed `user`: what follows a home directory's parent.
NOT_USER = r"(?!user(?![A-Za-z0-9._-]))"

# One row per rule: what it is called in a finding, and the pattern a line must not hold.
RULES = (
    ("an @", re.compile(r"@")),
    ("a home directory other than /home/user", re.compile(r"/home/" + NOT_USER)),
    ("a macOS home directory", re.compile(r"/Users/" + NOT_USER, re.IGNORECASE)),
    ("a Windows home directory", re.compile(r"[A-Za-z]:(?:\\\\|\\|/)+Users(?:\\\\|\\|/)+" + NOT_USER, re.IGNORECASE)),
    ("an escaped home directory", re.compile(r"-(?:home|Users)-(?!user(?:-|$|[^A-Za-z0-9._]))", re.IGNORECASE)),
    ("a dashless UUID", re.compile(r"(?<![0-9A-Fa-f])[0-9A-Fa-f]{32}(?![0-9A-Fa-f])")),
    (
        "a token",
        re.compile(r"glpat-|gh[pousr]_|github_pat_|ATATT|xox[bp]-|sk-ant-|(?i:bearer )|AKIA[0-9A-Z]{16}"),
    ),
    ("a private key", re.compile(r"-----BEGIN [A-Z0-9 ]*PRIVATE KEY-----")),
)


def findings(line: str) -> list[str]:
    """Returns the reasons @p line may not be committed in a fixture; empty when it may."""
    reasons = [name for name, pattern in RULES if pattern.search(line)]
    if any(not PLACEHOLDER_UUID.fullmatch(match.group(0)) for match in UUID.finditer(line)):
        reasons.append("a UUID that is not a placeholder")
    if any(not PLACEHOLDER_TOOL_USE_ID.fullmatch(match.group(0)) for match in TOOL_USE_ID.finditer(line)):
        reasons.append("a tool-use id that is not a placeholder")
    return reasons


def self_test() -> int:
    clean = (
        '{"sessionId":"00000000-0000-4000-8000-000000000001","cwd":"/home/user/proj"}',
        '{"cwd":"/home/user"}',
        "/home/user/proj/sub",
        '{"uuid":"00000000-0000-4000-8000-000000000012"}',
        '{"id":"toolu_000000000000000000000001"}',
        "plain text without anything personal",
        "/Users/user/proj and C:\\Users\\user\\proj and -home-user-proj",
        "a git object 4b825dc642cb6eb9a060e54bf8d69288fbee4904",
    )
    offending = (
        "someone@example.invalid",
        '{"uuid":"7da9cb6b-fec6-41fe-9c88-8a955b2975aa"}',
        "/home/alice/proj",
        "/home/username",
        "token glpat-x",
        "key sk-ant-x",
        "ghp_x gho_x github_pat_x",
        "ATATTx",
        "xoxb-x",
        "Authorization: Bearer x",
        "authorization: bearer x",
        '{"id":"toolu_01Qa7kf38XTBU4AajUeaQbUQ"}',
        "toolu_1",
        "/Users/alice/proj",
        "C:\\Users\\alice\\proj",
        "C:/Users/alice/proj",
        '"C:\\\\Users\\\\alice"',
        "projects/-home-alice-proj/",
        "projects/-Users-alice-proj/",
        "7da9cb6bfec641fe9c888a955b2975aa",
        "ghs_x",
        "ghu_x",
        "ghr_x",
        "github_pat_x",
        "AKIAABCDEFGHIJKLMNOP",
        "-----BEGIN OPENSSH PRIVATE KEY-----",
        "-----BEGIN PRIVATE KEY-----",
    )
    failures = [line for line in clean if findings(line)]
    failures += [line for line in offending if not findings(line)]
    for line in failures:
        print(f"self-test: wrong verdict for: {line}", file=sys.stderr)
    return 1 if failures else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--self-test", action="store_true", help="check the rules against inline samples")
    args = parser.parse_args()
    if args.self_test:
        return self_test()

    failed = False
    for tree in FIXTURE_TREES:
        root = REPO_ROOT / tree
        if not root.is_dir():
            continue
        for path in sorted(p for p in root.rglob("*") if p.is_file()):
            text = path.read_text(encoding="utf-8", errors="replace")
            for number, line in enumerate(text.splitlines(), 1):
                for reason in findings(line):
                    print(f"{path.relative_to(REPO_ROOT)}:{number}: {reason}")
                    failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
