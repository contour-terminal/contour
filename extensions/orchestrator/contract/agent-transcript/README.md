# `contour agent transcript`

What the orchestrator reads when it shows a run's transcript, and what Contour's verb prints. Both sides
test against the files in this directory: the orchestrator parses each `format-1/*.json` page, and
Contour's test runs the verb on the log each page names below and compares its output byte for byte.

## The command

```
contour agent transcript --session <uuid> --cwd <worktree> --page <n> --format <n> --json
```

- `--session` is the run's Claude Code session id, a canonical lower-case UUID; anything else is refused.
- `--cwd` is the run's worktree. The session's log (`<config>/projects/*/<uuid>.jsonl`, where `<config>` is
  `$CLAUDE_CONFIG_DIR`, else `$HOME/.claude`, on Windows `%USERPROFILE%\.claude` as Claude Code itself reads
  it) is this run's only when at least one of its entries carries a `cwd` and every one lies at or beneath
  `--cwd`, compared by whole path component — on Windows by drive or share too, and ignoring the case of ASCII
  letters; the worktree itself may be gone.
- `--page` is 1-based; page 0 and a page past the last are refused.
- `--format` is the format the caller reads. This document describes format **1**.
- Without `--json` the verb prints the same page as text, for a person, each time labelled `UTC`.

## Format 1

One JSON object, its keys in this order:

```
{"format":1,"session":"<uuid>","writtenBy":"<version>","page":1,"pages":3,"notUnderstood":0,
 "entries":[{"kind":"user","time":"2026-11-19T10:00:00.000Z","offset":60,"text":"…"}]}
```

| Key             | Meaning                                                                                    |
|-----------------|--------------------------------------------------------------------------------------------|
| `format`        | `1`.                                                                                       |
| `session`       | The session asked for.                                                                     |
| `writtenBy`     | The Claude Code version of the log's last shown entry; empty when there is none.           |
| `page`          | The page printed.                                                                          |
| `pages`         | How many pages the whole log has, at least 1 (an empty log is page 1 of 1).                |
| `notUnderstood` | The log's lines and blocks the verb could not place, counted over the whole log.           |
| `entries`       | The page's entries, at most 30, in log order.                                              |

`pages`, `notUnderstood` and `writtenBy` describe the whole log as read at the call; only the page's entries
are printed. Pages count shown entries only, so a page before the last never changes while the log grows.

An entry's `kind` is one of `user`, `assistant`, `tool-call` (`<name> <input as compact JSON>`),
`tool-result`, `tool-error` (a result marked as an error) and `note` (`conversation compacted`). A reader
counts a kind it does not know and skips it. `time` is the entry's ISO 8601 timestamp. `offset` is how many
minutes the local time of the host that printed the page was ahead of UTC at that instant, by its time zone's
rules, so that an entry written before a daylight-saving change shows the time it was then; it is left out
when `time` is not a UTC timestamp, and a reader shows such a time in UTC, labelled so.

### Limits

- Every text is sanitised line by line (no control character, no bidirectional or invisible formatting
  character) and its lines are joined with `\n`; the whole is cut to 2048 bytes on a code-point boundary.
- `time` and `writtenBy` are sanitised the same way, to at most 40 bytes.
- A log line longer than 1 MiB is skipped and counted, unless its type is one the verb ignores anyway.
- A last line without its newline is still being written: it is left for the next read.

### Refusals

The verb exits non-zero and prints one line on standard error. The one the format depends on is pinned:

```
unsupported transcript format <n>; this contour prints 1
```

A caller that gets it, or a page whose `format` is not the one it asked for, has a format mismatch: one
of the two sides needs updating. Never a parse error.

## Pages and the logs they were printed from

| Page                          | Log (in Contour's tree)                                                  |
|-------------------------------|--------------------------------------------------------------------------|
| `format-1/basic-page-1.json`  | `src/contour/cli/testdata/agent-transcript/2.1.328/basic.jsonl`, page 1  |
| `format-1/empty-page-1.json`  | `src/contour/cli/testdata/agent-transcript/synthetic/empty.jsonl`, page 1 |

Each was printed with `--session 00000000-0000-4000-8000-000000000001 --cwd /home/user/proj`, in UTC.

A new format is a new number: `format-2/`, beside this one, never an edit of a page here.
