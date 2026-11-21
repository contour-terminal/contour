# Synthetic session logs

Synthetic, written from the measured structure of the recorded log beside them (`../2.1.328/basic.jsonl`):
each holds the keys and types a real log carries, and only what one case of `AgentTranscript_test.cpp`
needs. None was recorded.

| File                    | Case                                                                         |
|-------------------------|------------------------------------------------------------------------------|
| `unknown-entries.jsonl` | an unknown type, a non-JSON line, an unknown block and system subtype        |
| `torn-tail.jsonl`       | a last line still being written (no newline)                                 |
| `compacted.jsonl`       | a compaction, its summary, an `isMeta` entry and a thinking block            |
| `empty.jsonl`           | metadata and an attachment only: no entry to show                           |
| `other-cwd.jsonl`       | written in another directory than the run's                                  |
| `subdir-cwd.jsonl`      | the agent went into subdirectories of the run's directory                    |
| `left-worktree.jsonl`   | one entry written outside the run's directory                                |
| `no-cwd.jsonl`          | metadata lines only, no entry carries a `cwd`                                |
| `prefix-sibling.jsonl`  | `/wt/ab`, a sibling of `/wt/a` sharing its prefix                            |
