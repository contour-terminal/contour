# `agent-runs.jsonl`

The file of finished agent runs a user analyses afterwards: one line per run, appended once the run finished
and its last statistics were read. The orchestrator builds each line (`core/Record.cpp`) and hands it to
`contour agent record --line <json> --format 1`, which checks that it is one JSON object of the format and
appends it, compactly, to `agent-runs.jsonl` in the daemon's state directory (`$XDG_STATE_HOME/contour`, else
`~/.local/state/contour`; `%LOCALAPPDATA%\contour` on Windows). The file is created private to the user and
never written through a link. A run stored before the file existed is appended once too.

## Format 1

One JSON object per line, its keys in this order:

| Key             | Meaning                                                                                  |
|-----------------|------------------------------------------------------------------------------------------|
| `format`        | `1`.                                                                                     |
| `run`           | The run's id, such as `pr-2379-20261113-150912`.                                         |
| `repo`          | The repository's configured name.                                                        |
| `item`          | `pr#<n>`, `issue#<n>`, `jira:<key>`, or `topic:<slug>` for a topic the user typed.        |
| `url`           | The item's web page; empty for a topic.                                                  |
| `task`, `model` | The task's id and the model the agent ran.                                               |
| `session`       | The agent's Claude Code session id; for a Copilot run, an id the orchestrator made, which names no Copilot session. |
| `dispatchedMs`, `endedMs` | When it was dispatched and ended, in milliseconds since the Unix epoch.        |
| `ended`         | How: `completed`, `ended-by-you`, `preparation-failed`, `never-reported`, `worktree-gone`, or `closed`: the tab of an agent that reports no status (Copilot) closed, a normal finish. |
| `worktree`      | `removed` or `kept`.                                                                     |
| `wallMs`        | From dispatch to the end, from the history; `null` without one.                          |
| `waitingMs`     | Time its agent was blocked, waiting for the user.                                        |
| `history`       | `{"wallMs","state"}` for each state it entered, at most 64, the first kept.              |
| `stats`         | `contour agent stats`'s report as last read (its format 1, without `format` and `session`), or `null`. |

A line is at most 8 KiB, the most a declared command's text argument holds: past that the history is left
empty first, then the statistics' tools and pull requests, then the statistics. A reader ignores a key it does
not know, and takes an `ended` value it does not know as some other end: values are added to it within this
format. A Copilot run's `stats` is `null`: nothing of it is read.

`format-1/finished.json` is the line of the run `core/Record_test.cpp` builds, byte for byte.

## Reading it

```sh
# What each task costs, on average
duckdb -c "select task, model, count(*) runs, round(avg(stats.cost.usd), 2) usd,
                  round(avg(wallMs) / 60000, 1) minutes
           from read_json('~/.local/state/contour/agent-runs.jsonl') group by all order by usd desc"

# The runs that waited longest for you
jq -r 'select(.waitingMs > 0) | [.run, (.waitingMs / 60000 | floor)] | @tsv' ~/.local/state/contour/agent-runs.jsonl
```

A new format is a new number: `format-2/`, beside this one, never an edit of a line here.
