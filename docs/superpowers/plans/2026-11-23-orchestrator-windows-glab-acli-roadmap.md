# Orchestrator on Windows, with GitLab (`glab`) and Jira (`acli`) — Roadmap

> Roadmap only: each phase gets its own implementation plan in this directory before it starts. Phase 1's is
> `2026-11-23-orchestrator-glab-acli.md`.

**Goal:** the agent orchestrator runs on Windows against GitLab repositories and Jira issues, as it runs on
Linux and macOS against GitHub today.

## Where it stands (measured 2026-11-23, Windows 11, glab 1.119.0, acli 1.3.41, gh 2.97)

| # | Piece | Today | Blocks |
| --- | --- | --- | --- |
| 1 | `glab` provider, `acli` issue source | `Verification::Unmeasured`: refused by the configuration, absent from the manifest | GitLab repos, Jira issues — every platform |
| A | `vtpty` directory opener (`DirectoryHandle_win32.cpp`) | **done**: handles open, resolve and create beneath on Windows (plan `2026-11-23-orchestrator-windows-directory-handles.md`); nothing remains for A | — |
| A2 | extension roots and environment on Windows (plan `2026-11-23-orchestrator-windows-extension-roots.md`) | `vtextension`'s root grammar refuses `C:\...` as `NotAbsolute`; `childEnvironment` compares variable names case-sensitively | resolving `@repos`/`@worktrees`, so every command |
| B | `vtpty::PipedProcessRunner` (`ProcessRunner_win32.cpp`; plan `2026-11-23-orchestrator-windows-process-runner.md`) | `start()` answers `Unsupported` | `git`, `gh`, `glab`, `acli`, `claude`, `contour agent …` |
| C | `file-read` beneath a root (`BeneathRead_win32.cpp`) | `not-supported-yet` (design sketched in the file) | the Board tab (`STATUS.md`) |
| D | `pane-open` in a held directory | unsupported on Windows | starting an agent |
| E | Packaging | Windows packages do not carry the orchestrator (`docs/extensions.md`, Limitations) | use outside a dev tree |

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

1. **GitLab + Jira recorded** (plan: `2026-11-23-orchestrator-glab-acli.md`). Platform-independent; verified
   by `orchestrator_test` and the manifest corpus test on Windows and Linux.
2. **A — Windows directory handles.** `DirectoryHandle` carries a native handle (`HANDLE` on Windows, fd on
   POSIX) behind one type; `open` = `CreateFileW(FILE_FLAG_BACKUP_SEMANTICS)`, `canonical` =
   `GetFinalPathNameByHandleW`, `createBeneath` = `NtCreateFile` relative to the parent handle with
   `FILE_OPEN_REPARSE_POINT`, refusing reparse points. Port `DirectoryHandle_test`.
   Gap left after it (plan `2026-11-23-orchestrator-windows-directory-handles.md`, "Out of scope"):
   `vtextension`'s root grammar is POSIX-only (`RootPointer.cpp`, `RootResolution.cpp` refuse `C:\...` as
   `NotAbsolute`), so `@repos`/`@worktrees` still do not resolve on Windows: phase A2.
   **Done**; the Windows differences it leaves are measured under "Known Windows differences" below.
   - **A2 — extension roots and environment on Windows** (plan
     `2026-11-23-orchestrator-windows-extension-roots.md`). A Windows path grammar for `vtextension`'s roots
     (`RootPointer.cpp`, `RootResolution.cpp` refuse `C:\...`), and case-insensitive variable names in
     `childEnvironment`. Needed before B–D are relied on for the orchestrator.
3. **B — Windows process runner** (plan `2026-11-23-orchestrator-windows-process-runner.md`).
   `CreateProcessW` suspended → Job Object (`KILL_ON_JOB_CLOSE`) → resume;
   overlapped pipes for stdout/stderr, pumped by `pump()` with the injected clock; PATH lookup over absolute
   entries with `PATHEXT` restricted to `.exe` (a `.cmd`/`.bat` is refused: no argument ever passes through
   `cmd.exe`); argv by `buildWindowsCommandLine()`; cwd from the handle's resolved path (the TOCTOU residual
   documented); the environment block built from `ProcessSpec::environment` only. Port the
   `ProcessRunner_test` cases that are not POSIX-signal specific.
   Gap to close with it: `src/vthost/StdioChild_win32.cpp` calls `CreateProcessW(..., TRUE, ...)` with
   inheritable pipes and no `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`, so its child inherits every inheritable
   handle other spawners have open at that moment; restrict it to an explicit handle list.
4. **C — Windows `file-read`.** Follow the design comment in `BeneathRead_win32.cpp`.
5. **D — Windows `pane-open`.** ConPTY in the held directory, through the runner's program lookup.
6. **E — Packaging and docs.** Ship the guest in Windows packages; drop the Windows limitation from
   `docs/extensions.md`; an end-to-end run on this machine: daemon, orchestrator, a GitLab repo, a JQL source.

## Pre-existing Windows test failures

Failing on Windows before phase 2 starts, none of them in the orchestrator: phases 2 to 6 must not count them
as regressions.

- `vtextension_test` does not compile: `popen` at `src/vtextension/DaemonGuest_test.cpp:708`.
- The `[consent]` case "DaemonExtensionRuntime: revoke…" fails.
- `vthost_test`: `DaemonConsentKey_test.cpp:140` fails.
- `contour_e2e_daemon_live_upgrade` times out waiting for the daemons to bind (20 s).
- `vthost_test`: `DaemonWorldE2E_test.cpp:196` fails (`status("orchestrator")` is empty).
- `vthost_test`: `StdioChild_test.cpp:145` fails: under Git Bash `cmd /c sort` resolves Git's `sort.exe`,
  which writes LF where the test expects CRLF.

`OwnedPanes_test.cpp:1171` (`REQUIRE( opened.has_value() )`), failing before phase A, passes since
it and is no longer listed.

Once `vtextension_test` compiles on Windows, its cases that `SKIP`ped on `GitSandboxError::Unsupported`
(e.g. `DeclaredCleanup_test.cpp:110`) run, since the Windows opener no longer answers `Unsupported`:
expect failures there until A2.

## Known Windows differences

- **A held directory's parent cannot be renamed.** NTFS refuses to rename a directory while any handle
  beneath it is open (`ERROR_ACCESS_DENIED`), whatever that handle shares; POSIX allows it. Removing the
  parent tree (`git worktree remove`) succeeds, and the held handle then reports `isRemoved()`. Pinned by
  `DirectoryHandle_test`, "a held directory's parent may be removed, but not renamed".
