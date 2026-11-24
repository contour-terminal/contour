# Orchestrator on Windows, with GitLab (`glab`) and Jira (`acli`) — Roadmap

> Roadmap only: each phase gets its own implementation plan in this directory before it starts. Phase 1's is
> `2026-11-23-orchestrator-glab-acli.md`.

**Goal:** the agent orchestrator runs on Windows against GitLab repositories and Jira issues, as it runs on
Linux and macOS against GitHub today.

**Status (2026-11-24): every phase is done.** Each row's commit range is written `first^..last`, both ends
inclusive. The boundary between A2 and C is inferred from commit order: `9e7bacef2` is counted as A2's last
commit, though it could belong to C. What is left is listed under "Out of scope" in each phase's plan;
the end-to-end run's manual rows are with the user (see "End-to-end run" below).

## Where it stands (measured 2026-11-23, updated 2026-11-24; Windows 11, glab 1.119.0, acli 1.3.41, gh 2.97)

| # | Piece | Today | Blocks |
| --- | --- | --- | --- |
| 1 | `glab` provider, `acli` issue source | **done**: recorded and enabled (plan `2026-11-23-orchestrator-glab-acli.md`): GitLab recorded from glab 1.119, Jira through acli recorded from acli 1.3.41, a harness that scrubs a recorded reply into a fixture, the configuration docs (`c73a476fc^..12372fdb8`) | — |
| A | `vtpty` directory opener (`DirectoryHandle_win32.cpp`) | **done** (plan `2026-11-23-orchestrator-windows-directory-handles.md`): the Windows path rules a handle obeys, open and resolve by handle, create beneath a held directory, hardened edges (`7e8a22f1a^..93bf86ac8`) | — |
| A2 | extension roots and environment on Windows | **done** (plan `2026-11-23-orchestrator-windows-extension-roots.md`): a root's path grammar for POSIX and Windows, roots resolved and judged in the opener's grammar, environment names folded, the orchestrator takes the host's spelling of a path, one Windows path corpus for host and guest, `vtextension_test` builds on Windows (`ee96175f4^..9e7bacef2`) | — |
| B | `vtpty::PipedProcessRunner` (`ProcessRunner_win32.cpp`) | **done** (plan `2026-11-23-orchestrator-windows-process-runner.md`): the rules a Windows spawn obeys as a pure unit, `.exe` programs run in a kill-on-close job each with both streams captured, timeout and cancel end the whole tree at once, background processes outlive a command that exits by itself (`6791c56fb^..aed9fbf41`) | — |
| C | `file-read` beneath a root (`BeneathRead_win32.cpp`) | **done** (plan `2026-11-23-orchestrator-windows-file-read.md`): what a Windows entry is to a read as a pure rule, the Win32 calls a read beneath a held directory makes, `file-read` on Windows through the native reader, a delete-pending directory is `Missing`, a removed root's handle let go of before it is opened again (`93561a1ff^..3d63d4b4b`) | — |
| D | `pane-open` in a held directory | **done** (plan `2026-11-23-orchestrator-windows-pane-open.md`): one check that a held directory can be a child's cwd, its name guarded until the child ended, one kill-on-close job for every held spawn, a pseudoconsole child started in a held directory, the agent's tree ends with its pane and survives an upgrade, a failed start closes its pseudoconsole (`18be32883^..d8f9c9cad`) | — |
| E | Packaging and docs | **done**: the MSI, per-user MSI and zip carry `share/contour/extensions/orchestrator.wasm` (plan `2026-11-24-orchestrator-windows-packaging-and-docs.md`): a warning when a program is no `.exe`, an NTSTATUS exit code shown in hex, the profile route and pseudoconsole-close fixes, the Windows CI wiring, the WIT, README and `docs/extensions.md` on Windows, a smoke script (`4e6cb637b^..51fc045b1`, plus this roadmap's own commits). The CI wiring (`beb16d479`) has **not run in CI yet**: nothing is pushed | — |

Findings from the recording that shaped phase 1:

- `glab issue list -F json` prints **text**: on `issue list`, `-F` is `--output-format` (details/ids/urls) and
  JSON is `-O/--output json`; on `mr list` and `repo view`, `-F` is `--output`. `--output json` is right for
  all three, and an empty issue list then prints `[]`.
- `glab mr list --output json` prints one compact JSON array carrying every field `GitLab.cpp` reads, no
  pipeline object, and `detailed_merge_status` values including `ci_still_running`, `draft_status`,
  `discussions_not_resolved`, `conflict`, `not_approved`.
- `acli jira workitem search --json` prints a pretty-printed JSON array; each item has `key` and
  `fields.summary` (plus `expand`, `id`, `self`, … at the top level); `[]` when nothing matches; exit 1 and
  a message on stderr for bad JQL. `acli jira auth status` exits 0 when logged in.

## Phases

1. **GitLab + Jira recorded** (plan: `2026-11-23-orchestrator-glab-acli.md`). **Done.** Platform-independent;
   verified by `orchestrator_test` and the manifest corpus test on Windows and Linux.
2. **A — Windows directory handles.** `DirectoryHandle` carries a native handle (`HANDLE` on Windows, fd on
   POSIX) behind one type; `open` = `CreateFileW(FILE_FLAG_BACKUP_SEMANTICS)`, `canonical` =
   `GetFinalPathNameByHandleW`, `createBeneath` = `NtCreateFile` relative to the parent handle with
   `FILE_OPEN_REPARSE_POINT`, refusing reparse points. Port `DirectoryHandle_test`.
   **Done**; the Windows differences it leaves are measured under "Known Windows differences" below.
   - **A2 — extension roots and environment on Windows** (plan
     `2026-11-23-orchestrator-windows-extension-roots.md`). **Done.** A Windows path grammar for
     `vtextension`'s roots (`RootPointer.cpp`, `RootResolution.cpp` no longer refuse `C:\...`), and
     case-insensitive variable names in `childEnvironment`.
3. **B — Windows process runner** (plan `2026-11-23-orchestrator-windows-process-runner.md`). **Done.**
   `CreateProcessW` suspended → Job Object (`KILL_ON_JOB_CLOSE`) → resume;
   overlapped pipes for stdout/stderr, pumped by `pump()` with the injected clock; PATH lookup over absolute
   entries with `PATHEXT` restricted to `.exe` (a `.cmd`/`.bat` is refused: no argument ever passes through
   `cmd.exe`); argv by `buildWindowsCommandLine()`; cwd from the handle's resolved path (the TOCTOU residual
   documented); the environment block built from `ProcessSpec::environment` only.
   Gap still open (raised in that plan's "Out of scope", a separate change): `src/vthost/StdioChild_win32.cpp`
   calls `CreateProcessW(..., TRUE, ...)` with inheritable pipes and no `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`, so
   its child inherits every inheritable handle other spawners have open at that moment.
4. **C — Windows `file-read`** (plan `2026-11-23-orchestrator-windows-file-read.md`). **Done.**
5. **D — Windows `pane-open`** (plan `2026-11-23-orchestrator-windows-pane-open.md`). **Done.** ConPTY in the
   held directory, through the runner's program lookup.
6. **E — Packaging and docs** (plan `2026-11-24-orchestrator-windows-packaging-and-docs.md`). **Done.** The guest
   ships in the Windows packages; `docs/extensions.md` no longer lists Windows as a limitation; the end-to-end
   run is below.

## Pre-existing Windows test failures

Measured by phase E's Task 1 (2026-11-24: `clangcl-release`, orchestrator guest on, the full `ctest`), none of
them attributed to phases A to E. Later work must not count them as regressions.

- `check_guest_portability`: 6 sites, from commits made before this roadmap (2026-11-22) — `at()` in
  `extensions/orchestrator/core/QuickReplies.cpp:59`, `extensions/orchestrator/guest/WitHost.cpp:140`,
  `extensions/orchestrator/view/Dashboard.cpp:1126`, `:1133`, `:1164`, and `vformat` in
  `extensions/orchestrator/view/Keys.cpp:753`. Not environmental.
- `vtextension_test`: `Bundled_test.cpp:272` and `DaemonGuest_test.cpp:844` fail. Not environmental.
- `vthost_test`: `StdioChild_test.cpp:145` fails. **Environmental**: run from Git Bash, `cmd /c sort` resolves
  Git's `sort.exe`, which writes LF where the test expects CRLF.
- `contour_test`: `AgentTranscript_test.cpp:773` (a recorded fixture) fails. Not environmental; it was never on
  this list before phase E measured it.
- `contour_windows_native_scaled_test`: device pixel ratio 2.25 where 1.5 is expected. **Environmental**: the
  monitor's scale.
- `contour_e2e_daemon_live_upgrade`: the daemons do not bind within 20 s. Not known to be environmental; the
  cause is not established.

Fixed, and no longer listed:

- `vtextension_test` did not compile (`popen` at `DaemonGuest_test.cpp:708`): it compiles since phase A2
  (`ee96175f4`). Its cases that `SKIP`ped on `GitSandboxError::Unsupported` now run.
- `vthost_test`: `DaemonConsentKey_test.cpp:140` passes since phase A2 (`be9305774`, the key files compared as a
  set).
- The `[consent]` case "DaemonExtensionRuntime: revoke…" passed in phase E's Task 1 run; the phase that fixed it
  was not isolated.
- `vthost_test`: `DaemonWorldE2E_test.cpp:196` (`status("orchestrator")` empty) passes since phase E
  (`17837faea`, `cd2f45969`, test-only: the world's paths were unescaped in its JSON, and a wait did not wait on
  Windows).
- `contour_test`: `DaemonSettings_test.cpp:297` and `:329` (a POSIX `/opt/...` path is not absolute on Windows)
  pass since phase E (`b5e8f104a`).
- `check_spelling` and `check_windows_macro_identifiers`, failing on this branch's own text, pass since phase E
  (`ccd04d8dc`).
- `OwnedPanes_test.cpp:1171` (`REQUIRE( opened.has_value() )`) has passed since phase A.

## Known Windows differences

Each is described for users in `docs/extensions.md`, section
[Daemon extensions on Windows](../../extensions.md#daemon-extensions-on-windows), except the YAML entry, which is in
[Configuring the orchestrator](../../extensions.md#configuring-the-orchestrator).

- **A held directory's parent cannot be renamed.** NTFS refuses to rename a directory while any handle
  beneath it is open (`ERROR_ACCESS_DENIED`), whatever that handle shares; POSIX allows it. Removing the
  parent tree (`git worktree remove`) succeeds, and the held handle then reports `isRemoved()`. Pinned by
  `DirectoryHandle_test`, "a held directory's parent may be removed, but not renamed".
- **Only `.exe` programs run**, looked up on `PATH`'s absolute entries; npm's `claude.cmd` is refused as not
  installed, and the configuration warns about a `programs` entry naming a batch file.
- **No graceful stop.** A timeout or cancel ends the whole process tree at once (no `SIGTERM`); a `git` cut off
  this way can leave `.git/index.lock` behind.
- **A held directory is entered by path**, checked by file identity before and after the process is created:
  renamed or replaced since it was opened ⇒ refused; a path longer than 258 characters is refused, never cut
  short.
- **The worktree cannot be removed or renamed while its agent runs** (nor any directory above it).
- **Job lifetime.** An agent and everything it starts run in one kill-on-close job, ended when the pane closes,
  when the daemon dies, or 5 s after its console closes; a live upgrade carries each job to the new daemon.
  A user's own shell runs outside any job, so what it starts outlives its tab.
- **`..` in a root is refused, not collapsed** (as are `.`, empty components, streams, device names, and names
  ending in a dot or a space).
- **SMB shares without file ids** cannot be read from by `file-read`.
- **YAML double-quoted Windows paths.** In a double-quoted YAML string `\U` starts an escape, so
  `"C:\Users\…"` does not parse; single-quote the path or use forward slashes. See
  [Configuring the orchestrator](../../extensions.md#configuring-the-orchestrator).

## End-to-end run (2026-11-24)

The smoke check is `scripts/smoke-orchestrator-windows.ps1` (`be9c48b84`, `51fc045b1`). It installs the build
into its own `-Root`, starts a daemon of its own there (socket pinned under `-Root`, `LOCALAPPDATA` overridden),
and never touches the user's daemon or configuration. `daemon stop` takes `--socket <Root>\daemon.sock`; the
client verbs take `--daemon <socket path>` (`--daemon NAME` names a saved host, not a daemon). The topic task's
key is `s`: `t` is reserved by the dashboard.

The run made: build `clangcl-release` (guest on), then the Offline tier unattended —

```
pwsh -NoProfile -File scripts/smoke-orchestrator-windows.ps1 -NoClient
pwsh -NoProfile -File scripts/smoke-orchestrator-windows.ps1 -Stop
```

| Automated check (Offline tier, `-NoClient`) | Result |
| --- | --- |
| Preflight: `contour.exe`, `orchestrator.wasm`, `git.exe`, `claude.exe` found | PASS |
| Install carries `share\contour\extensions\orchestrator.wasm` | PASS |
| Offline repository with a bare `file` origin | PASS |
| Configuration written (single-quoted Windows paths) | PASS |
| Daemon started in the background | PASS |
| `daemon.log` warns that a program which is no `.exe` is refused | PASS |
| `extension list`: `orchestrator  pending` | PASS |
| The consent question names the worktrees and repos directories in the long `C:\…` spelling, no `~1` | PASS |
| The module path ends in `share\contour\extensions\orchestrator.wasm` | PASS |
| `-Stop`: daemon stopped, `-Root` removed, no process left from it | PASS |
| `-Tier Forge` without its arguments refused before anything is written | PASS |

The manual rows 1–12 (grant in a client window; "Open Agents"; a topic run on key `s`; the `rename`, `board` and
`tree` probes; a live upgrade carrying the agent; ending a run; the Claude tier's board and transcript; the Forge
tier's MR and issue counts and a Jira-started run) were **handed to the user and not run**: the run was
unattended, with no GUI and no grant. No live agent tab has been tested on Windows yet. The script prints each
row's exact commands; start it without `-NoClient` (and with `-Tier Claude` or `-Tier Forge …` for rows 9–12),
and finish with `-Stop`.
