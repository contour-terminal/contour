# Orchestrator agent workers — design

## Intent

A user of the agent orchestrator wants to choose, per task, **which coding agent runs it and with what
environment**: two Claude Code setups side by side (one `CLAUDE_CONFIG_DIR` for work, another for personal
use), or GitHub Copilot CLI for some tasks. Today every run is `claude`, with the one environment the manifest
fixes, so a second account or a second agent is impossible without editing the manifest.

## Goals

- Named **workers** in the orchestrator's `config:`; each task names the worker that runs it.
- A worker runs with a named **variable set** the user defines; its values come from the user's configuration,
  never from the extension.
- A worker's set reaches **everything the orchestrator runs on its behalf**: the agent tab, the transcript
  reader, the status-hooks check, the stats reader, the plugin commands. A Claude worker with its own
  `CLAUDE_CONFIG_DIR` gets its own transcript (`t`) and its own hooks warning.
- Two worker kinds: `claude` and `copilot`.
- Every configuration valid today stays valid and behaves the same.

## Non-goals

- Status (working / blocked / done) and transcripts for Copilot runs. A Copilot run shows *running* until its
  tab closes; the user accepted this.
- Arbitrary variables. Only variables on a host list, and named by the manifest, can be set.
- Secrets in the configuration. No API-key variable is offered.
- Per-repository workers; workers in the daemon's `programs`.

## Anti-goals

- The extension must never choose a variable's **value**: a guest-chosen `ANTHROPIC_BASE_URL` or proxy would send
  the agent's login anywhere, and a guest-chosen `CLAUDE_CONFIG_DIR` could point at hooks that run programs.
- The consent dialog must still say exactly which variables, with which values, a tab or command can receive.
- The host's inheritance rules (`src/vtextension/ChildEnvironment.hpp`) and the manifest's own settable list
  (`SettableVariables`, `src/vtextension/SettableEnvironment.hpp`) keep their meaning; this adds beside them.

## Approaches

- **A — the manifest names the variables; their values come from the user (chosen).** The host interface (WIT)
  does not change, so no other extension rebuilds; the consent dialog lists each variable; a variable the
  manifest does not name needs an orchestrator release to add.
- **B — an open `env` list on `pane-open`/`proc-start`.** Any variable without a release, but a WIT change
  (every guest rebuilds) and a consent dialog that can no longer say what is set. Rejected.
- **C — variables on the daemon's `programs` entry.** Cannot differ between two Claude workers (the manifest
  names one `claude`). Rejected.

The user chose A. The builder check found that A as first drafted let the guest supply the values at run time,
which the host cannot tell from the user's own configuration. The user then chose that **the host fills the
values**: the user defines named sets in the extension's host entry, and the guest only picks a set by name.

## Design

### Configuration

```yaml
daemon:
    extensions:
        orchestrator:
            env_sets:                       # read by the host; shown in the consent dialog
                claude-work:     { CLAUDE_CONFIG_DIR: 'C:\Users\me\.claude-work' }
                claude-personal: { CLAUDE_CONFIG_DIR: 'C:\Users\me\.claude-personal' }
            config:
                workers:
                    - { id: claude-work,     agent: claude,  env_set: claude-work }
                    - { id: claude-personal, agent: claude,  env_set: claude-personal }
                    - { id: copilot,         agent: copilot }
                tasks:
                    - { id: topic, key: s, worker: claude-work, model: sonnet, prompt: "…" }
```

- **`env_sets`** (host, `src/contour/config`, beside `programs`): a map from a set name (an ArgumentKind::Name)
  to a map from a variable name to a value. Checked at config load: a variable not on the user-settable list, or
  a value of the wrong shape, is a configuration error naming the set and the variable; the set is kept without
  it. Written back by the config writer.
- **User-settable list** (host data, a new table beside `SettableVariables`): the variables a user's set may
  hold, each with its value shape:
  - `CLAUDE_CONFIG_DIR`, `COPILOT_HOME`: an absolute path (`~` expanded), normalised;
  - `ANTHROPIC_BASE_URL`: an `https://` URL, or `http://` to `localhost`, `127.0.0.1` or `::1`;
  - `HTTPS_PROXY`, `HTTP_PROXY`: a URL;
  - `NO_PROXY`: a comma-separated list of host names, domains or addresses.
  The manifest's own literal `pane-env`/`command-env` lines keep using only `SettableVariables`.
- **`workers[]`** (orchestrator `config:`): `id` (a Name, unique), `agent` (`claude` | `copilot`), `env_set`
  (optional, a set name, passed as is; the host checks it exists).
- **`tasks[].worker`**: a worker's id. Unknown → a configuration problem, and the first worker. Absent → the first
  worker. No `workers` → one implicit worker `claude` (agent `claude`, no set): today's behaviour exactly.

### Host (`src/vtextension`)

- New manifest lines `pane-env-set <pane> {<arg>} = <VAR> <VAR> …` and `command-env-set <command> {<arg>} =
  <VAR> …`: the pane or command takes a user set, chosen by the call's `<arg>` (an `arg` of kind `name`), and may
  receive only the listed variables from it. A listed variable not on the user-settable list, or a pane or command
  that does not exist, is a manifest error. The `<arg>` may be left out of a call; then no set applies.
- At `pane-open`/`proc-start` the host looks the set up in the calling extension's own `env_sets`. An unknown set
  fails the call (`bad-argument`). It applies the listed variables the set holds, re-checking each value's shape.
  A listed variable the set does not hold, and every other variable, follows the normal inheritance rules:
  **inherited from the daemon's environment unless withheld** (the user's choice for "not set").
- The consent dialog lists, per pane and command, the variables it may take from a set, and lists each configured
  set with its values. A set added, removed or changed after consent makes the extension ask again, through the
  same path a changed manifest digest takes.
- Tests: the manifest parser and the corpus (`contract/manifest-v2`, valid and invalid cases); config load of
  `env_sets`; the call path (unknown set, a set holding an unlisted variable, a wrong-shaped value after a reload,
  an omitted set); the consent text; asking again after a set changes.

### Manifest (`extensions/orchestrator`)

- `arg env-set = name`.
- `pane-env-set` on `agent` and `agent-plain` for `CLAUDE_CONFIG_DIR ANTHROPIC_BASE_URL HTTPS_PROXY HTTP_PROXY
  NO_PROXY`; `command-env-set` with the same list on every command that reads Claude's state: `agent-transcript`,
  `agent-setup-check`, `claude-plugins`, `claude-marketplace-add`, `claude-plugin-install`, and the stats command
  if the orchestrator runs one. `contour agent transcript`/`setup`/`stats` already honour `CLAUDE_CONFIG_DIR`
  (`src/contour/cli/AgentSessionLog.cpp`, `AgentStats.hpp`).
- A new pane `copilot`: `copilot --model {model} -i {prompt}`, with `pane-env-set` for `COPILOT_HOME HTTPS_PROXY
  HTTP_PROXY NO_PROXY`.

### Engine and dashboard (`extensions/orchestrator/core`)

- A run records its worker id and set name when it starts, persisted with the run. Every later call for that run
  (tab, transcript, hooks) passes that set name, even if the worker is later removed. A set the user has since
  removed makes those calls fail, with a problem line naming the set.
- Calls not tied to a run: the hooks check runs once per distinct set among the Claude workers, and its problem
  line names the worker (`Claude Code's status hooks are missing for worker claude-work: …`). Plugin commands run
  per Claude worker's set.
- Copilot runs: no `--session-id`, no `--settings`, no board prompt. Status is *running* while the tab is open.
  When the tab closes the run ends in a new end state **`Closed`**, which counts as a normal finish in the Finished
  list and the run statistics and has its own spelling in the stored-run format and `contour agent record`.
  `NeverReported` stays the state of a broken Claude run. `t` on a Copilot run says the transcript is unavailable.
  A task's `permissions` on a Copilot worker is a configuration problem (Copilot takes no profile).
- The dashboard shows a run's worker next to its model when more than one worker is configured.

### Docs

- `extensions/orchestrator/README.md`: workers, with the two-accounts example.
- `docs/extensions.md`: `env_sets`, the `pane-env-set`/`command-env-set` lines and the user-settable list. That
  file carries the user's uncommitted edits: its change goes through the index-only procedure used in phase E
  (commit against HEAD, merge into the working tree three-way, verify the user's hunks byte-identical).

## Constraints

- AGENT.md: C++23, `std::expected`, data-driven tables (one row per agent kind and per settable variable), enum
  class over bool, constructor-complete objects, tests with every change, zero warnings, clang-format.
- The orchestrator is also a wasm32-wasi guest: its code passes `check_guest_portability`.
- Windows: Copilot must be a real `copilot.exe`; npm's `copilot.cmd` is refused like `claude.cmd`, and the `.exe`
  warning at config load covers a `programs: copilot:` entry.
- No real user data in committed files; examples use `C:\Users\me\…`.

## Decisions the user made

- Workers live in the orchestrator's configuration; two Claude workers differ by `CLAUDE_CONFIG_DIR`.
- Copilot runs show *running* until the tab closes; no status or transcript.
- Approach A, with the host filling values from user-defined sets; the guest only names a set.
- A variable a worker's set does not hold is inherited from the daemon, as today.
- A Copilot run ends `Closed`, a normal finish.

## My calls (for the user to check)

- The variable list: `CLAUDE_CONFIG_DIR`, `ANTHROPIC_BASE_URL`, the three proxy variables, `COPILOT_HOME`; no
  API keys.
- The value shapes above, including `http://` only to localhost for `ANTHROPIC_BASE_URL`.
- A changed set re-asks for consent.
- An unknown `tasks[].worker` falls back to the first worker with a problem line, rather than refusing the task.

## Left to the builder

- Exact pane names, the per-agent table's shape, how the store persists a run's worker, and how Copilot's board
  text is passed (left out if it cannot be).
- Whether set values get a length limit beyond the config reader's.
