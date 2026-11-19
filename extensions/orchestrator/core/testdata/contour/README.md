# Recorded `contour` output

`workspace-list.json` and the two `setup-hooks-*.json` were recorded with the `contour` named in
`VERSION`. The `setup-hooks-*.json` were recorded again by Task 0.15's spike D with
`0.9.0-agent-orchestrator-f8d671a7d` and came out byte for byte the same, with the scratch home
replaced by `/home/user`:

- `setup-hooks-missing.json` — `contour agent setup dry-run json` with an empty `~/.claude` and
  `~/.agents`. With no `~/.claude` at all, `claude-code` reads `{"selection":"skipped"}` instead.
- `setup-hooks-present.json` — the same after a `contour agent setup json` into that home.
- `setup-claude-absent.json` — written by hand from the above, for a home with no `~/.claude` at all:
  `claude-code` reads `{"selection":"skipped"}`, which leaves the hooks Missing.

`status-{working,blocked,done}.json` are `pane.programStatusChanged` events as `contour events follow
json` prints them, recorded by spike D with `0.9.0-agent-orchestrator-f8d671a7d` and Claude Code
2.1.325 (`--model haiku --permission-mode default`), the hooks of `contour agent setup` installed.
Each carries two records: the integration's (`id` `claude-code`, `origin` `integration`), written by
the hooks through `contour agent report`, and Claude Code's own (`id` empty, `origin` `program`). The
three are one run: `working` while a Bash call runs, `blocked` (`kind` `permission`) at its
permission prompt, `done` after the turn ended.
