# `contour agent stats`

What the orchestrator reads when it records what a run cost and did, and what Contour's verb prints. Both
sides test against the files in this directory: the orchestrator parses each `format-1/*.json` report, and
Contour's test runs the verb on the log each report names below and compares its output byte for byte.

## The command

```
contour agent stats --session <uuid> --cwd <worktree> --format <n> --json
```

The session's log is found and bound to the run exactly as `contour agent transcript` finds it (see
[`../agent-transcript/README.md`](../agent-transcript/README.md)): `--session` must be a canonical lower-case
UUID, and every entry carrying a `cwd` must lie at or beneath `--cwd`; the worktree itself may be gone. The
logs of the session's subagents, `<session>/subagents/agent-*.jsonl` beside it, are read too, each bound to
`--cwd` the same way. Without `--json` the verb prints the same report as text, for a person.

## Format 1

One JSON object, its keys in this order:

| Key             | Meaning                                                                                       |
|-----------------|-----------------------------------------------------------------------------------------------|
| `format`        | `1`.                                                                                          |
| `session`       | The session asked for.                                                                        |
| `writtenBy`     | The Claude Code version of the log's last entry that names one; empty when none does.         |
| `first`, `last` | The ISO 8601 timestamps of the log's first and last entries that carry one; empty when none.  |
| `tokens`        | The main conversation's tokens, each API response counted once: `input`, `output`, `thinking` (part of `output`), `cacheWrite`, `cacheRead`. |
| `apiCalls`      | Its API responses: distinct message ids.                                                      |
| `apiErrors`     | API calls Claude Code recorded as failed; their tokens count nowhere.                         |
| `prompts`       | Prompts the user typed: the first and every answer after it, not a tool's result, not text Claude Code inserted. |
| `activeMs`      | The time Claude Code measured working on turns, summed.                                       |
| `toolCalls`     | Every tool call.                                                                              |
| `toolFailures`  | Tool results marked as errors.                                                                |
| `tools`         | `{"name","calls"}`, the most called first, ties by name; at most 16, while `toolCalls` counts all. |
| `compactions`   | Times the conversation was compacted.                                                         |
| `subagents`     | `{"count","apiCalls","tokens"}`: the subagent logs read, and what they spent.                 |
| `pullRequests`  | `{"number","url"}` for each pull request the session opened, at most 8.                       |
| `cost`          | What Claude Code recorded when the session last exited, or `null` while it runs: `usd` (rounded to a millionth; subagents and side calls included), `apiMs`, `apiMsWithoutRetries`, `toolMs`, `totalMs`, `linesAdded`, `linesRemoved`. |
| `notUnderstood` | Lines that did not parse or were longer than 1 MiB.                                           |

A reader ignores a key it does not know. Every text is sanitised (no control character, no bidirectional or
invisible formatting character): a tool's name, a time and a version to at most 64 bytes, a URL to 256.

### Refusals

As `contour agent transcript`'s, but for the format:

```
unsupported stats format <n>; this contour prints 1
```

A caller that gets it, or a report whose `format` is not the one it asked for, has a format mismatch: one of
the two sides needs updating. Never a parse error.

## Reports and the logs they were printed from

| Report                 | Log (in Contour's tree)                                              |
|------------------------|----------------------------------------------------------------------|
| `format-1/basic.json`   | `src/contour/cli/testdata/agent-stats/synthetic/basic.jsonl`        |
| `format-1/running.json` | `src/contour/cli/testdata/agent-stats/synthetic/running.jsonl`      |
| `format-1/empty.json`   | `src/contour/cli/testdata/agent-stats/synthetic/empty.jsonl`        |

Each was printed with `--session 00000000-0000-4000-8000-000000000001 --cwd /home/user/proj`.

A new format is a new number: `format-2/`, beside this one, never an edit of a report here.
