# D — Windows `pane-open` in a Held Directory Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** On Windows, `vtpty::Process` starts a program on a pseudoconsole in a directory held open
(`ExecInfo::directory`), so that `Process::heldDirectorySpawn()` answers `Supported`, `OwnedPanes::open()` stops
refusing `pane-open` with `NotSupported`, and the orchestrator can start `claude` in an agent's worktree pane.

**Architecture:** `Process_win32.cpp` gains a held-directory branch beside today's spawn ladder. It reuses phase B
whole: the program is found by `findWindowsHeldDirectoryProgram()` (`.exe` only, absolute PATH entries only, on the
PATH the child is given), the working directory is the held handle's `resolvedPath()` checked against the handle by
file id before and after `CreateProcessW` (the check phase B's runner makes, moved by Task 1 into a shared helper,
`Win32HeldDirectory`), and the child is created suspended, assigned to a kill-on-close Job Object and resumed. The
pseudoconsole attribute stays the only thing the child is handed (`bInheritHandles = FALSE`). The job is the
child's for its whole life: `terminate()` ends the tree, closing the console gives it Windows' console-close grace
before the job's last handle closes, and a live upgrade carries the job handle to the successor beside the process
handle (`AdoptedChild::job`).

**Tech Stack:** C++23, Win32 (`CreateProcessW` with `PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE`, Job Objects,
`GetFileInformationByHandleEx(FileIdInfo)`, thread-pool waits), phase B's `WindowsProcessRules`,
`process_runner_probe`, phase A's `DirectoryHandle::nativeHandle()`/`resolvedPath()`/`isRemoved()`, Catch2
(`vtpty_test`, `vthost_test`).

**Spec:** `docs/superpowers/plans/2026-11-23-orchestrator-windows-glab-acli-roadmap.md` (phase 5, "D — Windows
`pane-open`": "ConPTY in the held directory, through the runner's program lookup"). Builds on
`2026-11-23-orchestrator-windows-directory-handles.md` (A) and `2026-11-23-orchestrator-windows-process-runner.md`
(B).

## Depends on

- **Phase A** (landed).
- **Phase B Tasks 1, 2 and 3** must land first: Task 1's `WindowsProcessRules` (`findWindowsHeldDirectoryProgram`,
  `windowsImageName`), Task 2's `process_runner_probe` (the grandchild and `cwd`/`env` verbs these tests drive), and
  Task 3's file-id working-directory check, which Task 1 here moves out of `ProcessRunner_win32.cpp` into a shared
  helper. If phase B Task 3 is being implemented after this plan was read, have it call Task 1's helper instead
  of growing its own — one check, never two. Phase B Task 4 is not needed.
- **Phase A2 Task 8** (`paneWithheldNames` folds names on Windows) is not a build dependency, but without it a
  daemon's `Git_Dir` reaches the agent; land it before relying on this phase. For the orchestrator to reach
  `pane-open` at all on Windows, its roots must resolve: A2 Tasks 2–7 and 9–10.
- Independent of phase C (file-read).

## Facts that shaped this plan

- **Where it is refused today.** `src/vtpty/Process_win32.cpp:180` `Process::heldDirectorySpawn()` returns
  `Unsupported` ("No CreateProcess takes a directory by handle"); `Daemon.cpp:1748` wires it into
  `SessionHostConfig::heldDirectorySpawn`, so `OwnedPanes::open()` (`OwnedPanes.cpp:56`) answers
  `OwnedPaneError::NotSupported` → the guest's `not-supported-yet`. Behind it, `Process::start()`
  (`Process_win32.cpp:212`) refuses any `exec.directory` with `SpawnFailed` "A directory held open cannot be entered
  here."
- **The POSIX contract** (`Process_unix.cpp` `prepareTrampolineSpawn`): a held directory is entered by
  `fchdir()` in the trampoline; an unbacked handle or one `isRemoved()` is `SpawnFailed`; the program is found by
  `findHeldDirectoryProgram()` on the child's PATH's absolute entries and exec'd by path; the environment is the
  inherited one, less `removedEnvironment`, plus `env` — built in the parent, never by changing the parent's own.
  `SessionHost::createOwnedTab` asks for `StartDirectory::Required`: "in its directory or nowhere".
- **What the orchestrator starts** (`extensions/orchestrator/contract/manifest-v2/valid/orchestrator.manifest`):
  `pane agent @worktrees = claude --model {model} --session-id {session} --settings {settings}
  --append-system-prompt {board} {prompt}` and `agent-plain` without the board; `pane-env` sets only
  `GIT_CONFIG_*`. `{settings}` is a JSON object (`settingsJsonOf`, double quotes inside), `{board}` and `{prompt}`
  are free text with newlines; each `Text` argument is at most 8 KiB (`MaxTextArgumentBytes`), so an ordinary
  command line is well under `CreateProcessW`'s 32 766 UTF-16 units, and only a quote-dense board near its limit
  could pass it (refused `SpawnFailed`, never truncated).
- **`claude` on this machine** is `C:\Users\<user>\.local\bin\claude.exe` (phase B's measurement): the native
  installer's `.exe`, which the `.exe`-only lookup finds. npm's `claude.cmd` would be refused. Claude Code on
  Windows finds Git Bash through `CLAUDE_CODE_GIT_BASH_PATH` or `git` on PATH: neither is withheld from a pane
  (only `GIT_*`, `GH_*`, `GLAB_*`, `CONTOUR_*`-family names are, by `InheritanceRules`), and it needs
  `USERPROFILE`/`APPDATA`/`LOCALAPPDATA`/`SystemRoot`, which are inherited.
- **Windows panes survive a live upgrade by handle inheritance** (`ImageHandoff`, `HandoverCodec.cpp:240`
  `visitForeignFields(AdoptedChild)`, `HandoverRestore.cpp` `handlesNamedBy`): any per-child handle that must
  outlive the old daemon — the job — has to cross the same way, or the old daemon's exit kills the agent.
- **NTFS refuses to rename a directory while anything beneath it is open** (phase A's
  `DirectoryHandle_test`, "a held directory's parent may be removed, but not renamed"); the held directory itself
  may be renamed. So the only swap of `resolvedPath()` that can happen is the held directory renamed away and
  another put at its name — exactly what the file-id check detects.

## Global Constraints

- Project rules in `AGENT.md`: C++23, `std::expected`, `enum class` over `bool` (new enums `: uint8_t`), Doxygen on
  every new public declaration, `.hpp` + `#pragma once`, no C-style loops, no `NOLINT`, zero warnings under
  `-Werror` and clang-tidy, data-driven tables (refusal → detail text, verbs of the probe).
- Never edit `vendor/core-cpp`.
- **POSIX behaviour is unchanged.** `Process_unix.cpp`, `ProgramLookup.cpp` and POSIX tests are not edited; the
  `AdoptedChild::job` field is `none` on POSIX and encodes as a none handle. Linux CI is the gate for that half.
- **User shells are unchanged**: a spawn without `ExecInfo::directory` keeps today's path (spawn ladder, no job,
  `CreateProcessW` with a null application name).
- **No program runs through `cmd.exe`**, and only an absolute `.exe` is ever started for a held directory (phase B
  Decision 4/5).
- **No handle leaks into the agent**: `bInheritHandles` stays `FALSE`; every handle this phase makes (job,
  verification opens) is non-inheritable.
- Do not edit `docs/extensions.md` or the roadmap file (uncommitted user work); doc text goes in "Docs for phase E".
- Do not run two builds in one tree at once; 10-minute timeout. Commands:
  - `cmake --build --preset clangcl-release --target vtpty_test vthost_test`
  - binaries `out/build/clangcl-release/bin/vtpty_test.exe`, `out/build/clangcl-release/bin/vthost_test.exe`
- Every commit message ends with:
  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  ```

## Decisions

1. **The working directory is `resolvedPath()`, verified by file id before and after `CreateProcessW` — phase
   B's Decision 7, through one shared helper.** `CreateProcessW` takes a path; the POSIX `fchdir` guarantee cannot
   be had. The path is opened (`FILE_READ_ATTRIBUTES`, `FILE_FLAG_BACKUP_SEMANTICS`, share-all) and its
   `FILE_ID_INFO` compared with the held handle's, once before creation and once after it with the child still
   suspended; a mismatch terminates the suspended child. Residual TOCTOU (documented at the helper, as in phase
   B): the name swapped and swapped back within the `CreateProcessW` call goes unseen. Consequence: **a held
   directory renamed since it was opened is refused, not entered at its new name**, where POSIX enters it wherever
   it went. Reading the path afresh from the handle at spawn time would follow a rename; it is not done, to keep
   one rule with phase B's runner.
2. **A held directory too long to be a current directory is refused, not truncated.** Win32 limits a process's
   current directory to `MAX_PATH - 2` (258) UTF-16 units; a longer `resolvedPath()` is `SpawnFailed` before
   anything is created (A2's "Out of scope" asked phase B for this; the shared helper gives both).
3. **Program lookup is phase B's `findWindowsHeldDirectoryProgram()` on the PATH of the environment the child is
   given** (`buildWindowsEnvironment(inherited, env, removed)`, read case-insensitively), with
   `NativeFileSystem::isExecutableFile` on each absolute candidate. The found absolute path is
   `lpApplicationName`; the command line is `buildWindowsCommandLine(exec.program, exec.arguments)` — argv[0] as
   declared, as phase B Decision 6. `Process::findsProgram()` answers by the same call for an `ExecInfo` with a
   directory, so `pane-open`'s `ProgramMissing` and the spawn agree. A held spawn's program is never read as a
   command line (no whitespace splitting, `invocationToRun` is not applied): it comes from a manifest argv token.
4. **No spawn ladder for a held directory.** Today's Windows start falls back to dropping the working directory
   and then to the login shell; a held spawn makes one attempt — the program, in the directory — or fails, as
   `StartDirectory::Required` and POSIX's trampoline demand. A fallback would show a PowerShell in the worktree
   where an agent was expected.
5. **The environment is the spec's: inherited, less `removedEnvironment`, plus `env`**, built in the daemon by
   `buildWindowsEnvironment` (names compared case-insensitively) and handed over as a block — the POSIX rule, and
   what today's Windows spawn already does. The daemon's own environment is never modified.
6. **`bInheritHandles = FALSE`, not a `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`.** A handle list only narrows
   inheritance when `bInheritHandles` is `TRUE`; with `FALSE` nothing at all is inherited, and the console comes
   from the pseudoconsole attribute. That is strictly tighter than any list, and it is what `Process_win32.cpp`
   already does — kept, and now tested.
7. **A held-directory child runs in its own kill-on-close Job Object; a user's shell runs in none.** Created
   suspended (`CREATE_SUSPENDED`), assigned, resumed, as phase B's runner. No `JOB_OBJECT_LIMIT_BREAKAWAY_OK`. The
   job is the agent's process tree — `claude`, the `node`/MCP servers and shells it starts, including ones that
   detach from the console. User shells stay job-free because a user's `code .` or `explorer .` must outlive the
   tab; an agent's helpers must not outlive the agent. **Known cost:** a browser the agent launches fresh (for a
   login) is in the job and ends with the pane; one already running is not.
8. **How the job ends.** `terminate()` → `TerminateJobObject` (the tree, at once — Windows has no hangup, as
   today's `TerminateProcess`). `~Process` with the leader still running and the console closed → the job's
   handle is handed to a thread-pool wait that closes it when the leader exits or after `ConsoleCloseGrace` (5 s,
   the time Windows itself gives a console process to handle `CTRL_CLOSE_EVENT`), so `claude` can handle the
   console close before stragglers are killed. The daemon dying in any way closes the handle → the tree ends.
   `HangupGrace` stays unused on Windows, as today.
9. **The job crosses a live upgrade with the child.** `AdoptedChild` gains `CrossingHandle job` (handover field
   7, a none handle when absent — an image from an older daemon decodes with no job and the child keeps running
   job-less); `handoverChild()`/`releaseChild()` fill it, `handlesNamedBy()` names it, `Process::adopt()` takes it
   (non-inheritable again, as it does the process handle). Without this, the old daemon's exit would close the
   last job handle and kill every agent mid-upgrade.
10. **`Process::heldDirectorySpawn()` answers `Supported` on Windows for every `SandboxEscape`** (Windows has no
    sandbox), flipped only in the last task, once start() enters held directories — so no build in between
    admits a `pane-open` that would fail.

## Review Focus

Security first. Invariants: **the agent starts in the held directory or not at all, runs the `.exe` the user
installed and nothing planted in the worktree, inherits no daemon handle, and its whole tree ends with the pane
and with the daemon.**

- **TOCTOU on the directory.** Replaced at its name (rename + new directory, rename + junction to elsewhere) →
  refused, the planted marker never written; removed → refused; the after-creation check runs with the child
  suspended and terminates it on mismatch. Task 1 (helper), Task 3 (`Process`).
- **Link-following.** A junction put at the held directory's name is a different file id → refused (Task 1). The
  program found on PATH is a regular `.exe` by absolute path; nothing in the worktree is searched (Task 3 "a
  program planted in a held directory is never found on the PATH").
- **8.3 / case / device names.** The program name goes through `windowsImageName()` (`.cmd`, `.bat`, `.com`,
  trailing dot refused); the cwd is the handle's canonical long-name spelling (phase A); a held directory reached
  through a short name was canonicalised when opened. Task 3 ("a batch file is never run in a held directory").
- **Oversized input.** A command line past 32 766 units and a cwd past 258 units → refused before
  `CreateProcessW`, never truncated (Tasks 1, 3).
- **Handle leaks.** An inheritable pipe end made by the test (as the daemon's `StdioChild` makes them) does not
  reach the agent: its read end sees `ERROR_BROKEN_PIPE` once the test closes its write end (Task 3). The job and
  verification handles are non-inheritable (Task 1, Task 4).
- **Children outliving the daemon.** `terminate()` kills a detached grandchild (Task 4); the job's last handle
  closing kills it (Task 4, standing in for the daemon's death, which closes every handle); the console-close
  path kills it within the grace (Task 4); a live upgrade does *not* kill it, and the successor's `terminate()`
  does (Task 4, adopt).
- **No fallback.** A missing program or refused directory never starts the login shell (Task 3).
- **POSIX parity.** No POSIX file or test changes except the defaulted `AdoptedChild::job` and the codec field.

---

### Task 1: One held-directory entry check for every Windows spawn

**Files:**
- Create: `src/vtpty/Win32HeldDirectory.hpp` (whole header `#ifdef _WIN32`, includes `DirectoryHandle.hpp`,
  `UniqueHandle.hpp`, `<Windows.h>`), `src/vtpty/Win32HeldDirectory.cpp`
- Modify: `src/vtpty/WindowsProcessRules.hpp` (phase B Task 1's pure unit: two limits, below)
- Modify: `src/vtpty/ProcessRunner_win32.cpp` (phase B Task 3: delete its own file-id check and
  `MaxCommandLine`; call the helper; its error mapping gains the `TooLong`/`NotUtf8` rows)
- Modify: `src/vtpty/CMakeLists.txt` (`Win32HeldDirectory.cpp` in the Windows source block, header in
  `vtpty_HEADERS` there; `Win32HeldDirectory_test.cpp` in the `if(WIN32)` test sources)
- Create: `src/vtpty/Win32HeldDirectory_test.cpp` (whole file `#ifdef _WIN32`)
- Modify: `src/vtpty/ProcessRunner_win32_test.cpp` (one case)
- Create (unless phase C Task 2 already did): `src/vtpty/test/Win32Junction.hpp` — `MountPointHeader` and
  `createJunction()` moved verbatim out of `DirectoryHandle_test.cpp`'s anonymous namespace into
  `namespace vtpty::testing` as `inline`, and `DirectoryHandle_test.cpp` switched to it. Whichever of C and D
  lands first moves it; the other only includes it.

**Interfaces:**

```cpp
// WindowsProcessRules.hpp (pure, every platform)
/// The longest command line CreateProcessW takes, in UTF-16 units.
inline constexpr std::size_t MaxWindowsCommandLineUnits = 32'766;
/// The longest current directory a Windows process may have, in UTF-16 units: MAX_PATH less the trailing `\`
/// and the NUL.
inline constexpr std::size_t MaxWindowsCurrentDirectoryUnits = 258;

// Win32HeldDirectory.hpp (namespace vtpty)
/// Why a held directory cannot be a child's current directory.
enum class HeldDirectoryEntryError : uint8_t
{
    Unbacked, ///< The handle holds no directory (DirectoryHandle::unbacked).
    Removed,  ///< The directory was removed while held.
    Replaced, ///< Its resolved path names another directory now (or nothing).
    TooLong,  ///< Its resolved path is longer than MaxWindowsCurrentDirectoryUnits.
    NotUtf8,  ///< Its resolved path is not UTF-8.
};

/// A held directory made ready to be passed to CreateProcessW as lpCurrentDirectory.
struct HeldDirectoryEntry
{
    std::wstring path;      ///< resolvedPath(), UTF-16: what CreateProcessW is given.
    FILE_ID_INFO identity;  ///< The held handle's volume serial and file id.
};

/// Prepares @p held to be a child's current directory: its path, checked to name the held directory now.
/// CreateProcessW takes a path, not a handle: call confirmHeldDirectoryEntry() again once the child is created
/// suspended. Residual: the name swapped away and back within the CreateProcessW call is not seen.
/// @param held The directory, held open.
/// @return What to pass to CreateProcessW; or why it cannot be.
[[nodiscard]] std::expected<HeldDirectoryEntry, HeldDirectoryEntryError> prepareHeldDirectoryEntry(
    DirectoryHandle const& held);

/// @param entry What prepareHeldDirectoryEntry() made.
/// @return Nothing when entry.path still names the directory of entry.identity; Replaced otherwise.
[[nodiscard]] std::expected<void, HeldDirectoryEntryError> confirmHeldDirectoryEntry(
    HeldDirectoryEntry const& entry);
```

Order in `prepare`: `nativeHandle() == nullptr` → `Unbacked`; `isRemoved()` → `Removed`; `core::toUtf16` fails →
`NotUtf8`; length > limit → `TooLong`; `FileIdInfo` of the handle (failure → `Replaced`: unprovable is refused);
then `confirmHeldDirectoryEntry`. The verification open is non-inheritable and closed before returning.

- [ ] **Step 1: Write the failing tests** — `Win32HeldDirectory_test.cpp`, tag `[processrunner][held]`:

```cpp
TEST_CASE("HeldDirectoryEntry: a held directory is its own entry", "[processrunner][held]")
{   // open temp\work; prepare → path == toUtf16(resolvedPath()); confirm → ok
}
TEST_CASE("HeldDirectoryEntry: a directory replaced at its name is refused", "[processrunner][held]")
{   // open work; rename work → work.old; then, by SECTION:
    //   a new directory at `work`                         → prepare == Replaced
    //   createJunction(work, elsewhere)                   → prepare == Replaced
    //   createJunction(work, work.old) (back to itself)   → ok: the same file id, so the child's cwd is the
    //                                                       held directory (documented, not a refusal)
}
TEST_CASE("HeldDirectoryEntry: a directory replaced after it was prepared is refused at the confirmation", "[processrunner][held]")
{   // prepare ok; rename work → work.old; create work → confirm == Replaced
}
TEST_CASE("HeldDirectoryEntry: a removed directory is refused", "[processrunner][held]")
TEST_CASE("HeldDirectoryEntry: an unbacked handle is refused", "[processrunner][held]")
TEST_CASE("HeldDirectoryEntry: a path too long to be a current directory is refused, not cut", "[processrunner][held]")
{   // nested directories under temp until the canonical path exceeds 258 UTF-16 units (created through
    // `\\?\`), opened by the native opener → prepare == TooLong; one of exactly 258 units → ok
}
```

and in `ProcessRunner_win32_test.cpp`:

```cpp
TEST_CASE("PipedProcessRunner: a working directory too long to enter is refused", "[processrunner]")
{   // the 259-unit directory → start == ProcessStartError::SpawnFailed, nothing ran (a marker verb never ran)
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build --preset clangcl-release --target vtpty_test`
Expected: compile error, `'vtpty/Win32HeldDirectory.hpp' file not found`.

- [ ] **Step 3: Implement** the helper; switch the runner to it (`Unbacked`/`TooLong`/`NotUtf8` → `SpawnFailed`,
  `Removed`/`Replaced` → `DirectoryMissing`, as one table), deleting its private copy and `MaxCommandLine` (use
  `MaxWindowsCommandLineUnits`).

- [ ] **Step 4: Run to verify they pass**

Run: `cmake --build --preset clangcl-release --target vtpty_test && out/build/clangcl-release/bin/vtpty_test.exe "[held]" && out/build/clangcl-release/bin/vtpty_test.exe "[processrunner]"`
Expected: `All tests passed` for both; every phase B runner case still passes.

- [ ] **Step 5: Commit**

```bash
git add src/vtpty/Win32HeldDirectory.hpp src/vtpty/Win32HeldDirectory.cpp src/vtpty/Win32HeldDirectory_test.cpp src/vtpty/WindowsProcessRules.hpp src/vtpty/ProcessRunner_win32.cpp src/vtpty/ProcessRunner_win32_test.cpp src/vtpty/CMakeLists.txt src/vtpty/test/Win32Junction.hpp src/vtpty/DirectoryHandle_test.cpp
git commit -m "refactor(vtpty): one check that a held directory can be a Windows child's cwd

The runner's file-id check moves into Win32HeldDirectory, for the
pseudoconsole spawn to share: the resolved path must name the held
directory before and after CreateProcessW, and a path longer than a
current directory may be is refused rather than cut.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: The probes say which file ran and report into a file

**Files:**
- Modify: `src/vtpty/test/conpty_osc133_probe.cpp` (a `module: [<hex>]` line: `GetModuleFileNameW(nullptr)`,
  reported like `directory`; header comment updated)
- Modify: `src/vtpty/test/process_runner_probe.cpp` (two verb rows, below)
- Modify: `src/vthost/CMakeLists.txt` (`vthost_test`: `add_dependencies(vthost_test process_runner_probe)` and
  `VTHOST_PROCESS_RUNNER_PROBE="$<TARGET_FILE:process_runner_probe>"` under `if(WIN32)`)
- Test: `src/vtpty/CommandCapture_test.cpp` (phase B's `[probe]` smoke case gains two checks)

New probe verbs (rows in its `constexpr` verb table):

| verb | effect |
| --- | --- |
| `report <file>` | from here on, everything the verbs write goes to `<file>.part`, moved to `<file>` (`MoveFileExW`, replace) when the probe exits normally |
| `spawn-detached <file> …` | starts itself with the remaining verbs under `DETACHED_PROCESS \| CREATE_NEW_PROCESS_GROUP`, standard handles `NUL`, nothing inherited; writes the grandchild's pid to `<file>`; does not wait — a grandchild no console close reaches, only a job |

- [ ] **Step 1: Extend the smoke test** (`[processrunner][probe]`): `"<probe>" report <tmp>\r args a` → `<tmp>\r`
  holds `a\n` after the capture ends; `"<probe>" spawn-detached <tmp>\g pid <tmp>\gp sleep 2000` → both pid files
  appear within 5 s and name the same live process. And in `ConPty_test.cpp`'s probe helpers, a
  `printedFields(output, "module")` check in the existing "childReceivesItsProgramPathAndArgumentsIntact" case:
  equals the probe's path.
- [ ] **Step 2: Run to verify it fails** — `vtpty_test.exe "[probe]"`: the unknown verb exits non-zero.
- [ ] **Step 3: Implement** the rows and the `module` line.
- [ ] **Step 4: Run** `cmake --build --preset clangcl-release --target vtpty_test vthost_test && out/build/clangcl-release/bin/vtpty_test.exe "[probe]" && out/build/clangcl-release/bin/vtpty_test.exe "ConPty*"` → `All tests passed`.
- [ ] **Step 5: Commit**

```bash
git add src/vtpty/test/conpty_osc133_probe.cpp src/vtpty/test/process_runner_probe.cpp src/vtpty/CommandCapture_test.cpp src/vtpty/ConPty_test.cpp src/vthost/CMakeLists.txt
git commit -m "test(vtpty): probes that name the file that ran and report into a file

The pseudoconsole probe prints its own module path; the runner probe can
write its output to a file and start a detached grandchild, which only a
job object can end.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: `Process::start()` in a held directory

**Files:**
- Modify: `src/vtpty/Process_win32.cpp` (the held branch; `terminate()`; the job member — its *lifetime* rules
  are Task 4)
- Modify: `src/vtpty/Process.hpp` (Doxygen only: `ExecInfo::directory` — Windows: "started by its resolved path,
  verified by file id before and after creation; refused when renamed since it was opened"; `terminate()` — "a
  held-directory child's whole job")
- Test: `src/vtpty/ConPty_test.cpp` (new cases beside "childIsFoundOnThePathConfiguredForIt", reusing `runProbe`,
  `printedFields`, `probeCopiedInto`)

**Shape** (anonymous namespace; names indicative):

```cpp
/// Why a held-directory spawn did not start, as the detail StartFailure carries: one row per refusal.
constexpr auto HeldEntryDetails = std::to_array<std::pair<HeldDirectoryEntryError, std::string_view>>({
    { HeldDirectoryEntryError::Unbacked, "The directory \"{}\" cannot be entered by its handle here." },
    { HeldDirectoryEntryError::Removed,  "The directory \"{}\" was removed." },
    { HeldDirectoryEntryError::Replaced, "The directory \"{}\" is no longer the one that was opened." },
    { HeldDirectoryEntryError::TooLong,  "The directory \"{}\" is too long to start a program in." },
    { HeldDirectoryEntryError::NotUtf8,  "The directory \"{}\" is not UTF-8." },
});

/// Starts @p exec in its held directory on @p pty, or nothing (Decisions 1–7).
[[nodiscard]] StartResult startInHeldDirectory(Process::Private& d);
```

`startInHeldDirectory`, in order — every early return leaves no process and no handle:
1. `prepareHeldDirectoryEntry(*exec.directory)` → refusal → `SpawnFailed` with its row's detail.
2. `buildWindowsEnvironment(inherited, exec.env, exec.removedEnvironment)` → block (not UTF-8 → `SpawnFailed`).
3. `findWindowsHeldDirectoryProgram(exec.program, PATH of that environment, isExecutable)` → none →
   `SpawnFailed` "The program \"{}\" was not found." (POSIX's wording).
4. `buildWindowsCommandLine(exec.program, exec.arguments)` → UTF-16; longer than `MaxWindowsCommandLineUnits` →
   `SpawnFailed`.
5. Job: `CreateJobObjectW(nullptr, nullptr)` + `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`.
6. `pty->start()`; `initializeStartupInfoAttachedToPTY`.
7. `CreateProcessW(program, commandLine, nullptr, nullptr, FALSE, EXTENDED_STARTUPINFO_PRESENT |
   CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED, block, entry.path, …)`; failure → `SpawnFailed` with
   `systemErrorMessage`.
8. `confirmHeldDirectoryEntry(entry)`; then `AssignProcessToJobObject`, `ResumeThread`; any failure →
   `TerminateProcess(process, 1)`, close both handles, `SpawnFailed`.
9. Keep the job in `Private`; `watchExit`; return `StartOutcome {}` (empty diagnostic: nothing was given up).

`start()` calls it when `exec.directory` is set, before any of today's ladder code. `terminate()` uses
`TerminateJobObject(job, 1)` when `Private` holds a job, else `TerminateProcess` as today.

- [ ] **Step 1: Write the failing tests** in `ConPty_test.cpp`, tag `[vtpty][conpty][held]`. Fixture helper
  `heldExec(probeCopy, work, args)`: `execInfo("<probe stem>", args)`, `env = { {"Path", bin} }`,
  `workingDirectory = work`, `directory = shared DirectoryHandle of work`.

```cpp
TEST_CASE("ConPty.childStartsInItsHeldDirectory", "[vtpty][conpty][held]")
{   // probe copied into bin; runProbe(heldExec(...)) → printedFields "directory" == { held->resolvedPath() },
    // "module" == { bin\probe.exe }, started->diagnostic empty
}
TEST_CASE("ConPty.aProgramPlantedInAHeldDirectoryIsNeverFoundOnThePath", "[vtpty][conpty][held]")
{   // the probe also copied as work\probe.exe and work\bin\probe.exe; Path = GENERATE(";<bin>", ".;<bin>",
    // "bin;<bin>", "C:;<bin>") → "module" == { <bin>\probe.exe }
}
TEST_CASE("ConPty.aBatchFileIsNeverRunInAHeldDirectory", "[vtpty][conpty][held]")
{   // bin\tool.cmd writing a marker; exec program "tool" and "tool.cmd" → start() fails SpawnFailed, detail
    // names the program; the marker never exists; no child (alive() false) — no login-shell fallback
}
TEST_CASE("ConPty.aMissingProgramInAHeldDirectoryDoesNotFallBack", "[vtpty][conpty][held]")
{   // program "contour-no-such-program" → SpawnFailed "The program \"contour-no-such-program\" was not found.";
    // alive() false
}
TEST_CASE("ConPty.aHeldDirectoryReplacedAtItsNameStartsNothing", "[vtpty][conpty][held]")
{   // open work; rename work → work.old; create work (section 2: a junction work → elsewhere) →
    // SpawnFailed "… is no longer the one that was opened."; alive() false
}
TEST_CASE("ConPty.aRemovedOrUnbackedHeldDirectoryStartsNothing", "[vtpty][conpty][held]")
{   // removed → "was removed."; DirectoryHandle::unbacked(work) → "cannot be entered by its handle here."
}
TEST_CASE("ConPty.anAgentsArgumentsReachItExactly", "[vtpty][conpty][held]")
{   // arguments as the orchestrator builds them: "--model", "opus", "--session-id", a UUID, "--settings",
    // R"({"permissions":{"defaultMode":"acceptEdits","allow":["Bash(git *)"]}})", "--append-system-prompt",
    // "line one\nline \"two\" with \\ and %PATH% & | ^ <>", "größer – 日本" → printedArguments == the same
}
TEST_CASE("ConPty.anOverlongCommandLineInAHeldDirectoryStartsNothing", "[vtpty][conpty][held]")
{   // one argument of 33 000 'x' → SpawnFailed; alive() false
}
TEST_CASE("ConPty.aHeldDirectoryChildIsHandedTheEnvironmentBuiltForIt", "[vtpty][conpty][held]")
{   // as "childIsHandedTheEnvironmentBuiltForIt", through heldExec: CONTOUR_PROBE_INHERITED kept,
    // CONTOUR_PROBE_DROPPED withheld by its lower-case spelling, CONTOUR_PROBE_SET configured
}
TEST_CASE("ConPty.anInheritableHandleOfTheDaemonDoesNotReachAHeldDirectoryChild", "[vtpty][conpty][held]")
{   // CreatePipe(&read, &write, &inheritable /*bInheritHandle TRUE*/, 0); start heldExec(probe, {"--linger-on-close"})
    // and await its sentinel; CloseHandle(write); PeekNamedPipe(read, …) fails with ERROR_BROKEN_PIPE;
    // terminate
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build --preset clangcl-release --target vtpty_test && out/build/clangcl-release/bin/vtpty_test.exe "[held]"`
Expected: the new ConPty cases fail with today's detail "A directory held open cannot be entered here."; Task 1's
cases still pass.

- [ ] **Step 3: Implement** as described.

- [ ] **Step 4: Run to verify they pass**

Run: `cmake --build --preset clangcl-release --target vtpty_test && out/build/clangcl-release/bin/vtpty_test.exe "[held]" && out/build/clangcl-release/bin/vtpty_test.exe "ConPty*"`
Expected: `All tests passed` for both (10 new ConPty cases; every existing ConPty case unchanged — user shells take
the old path).

- [ ] **Step 5: Commit**

```bash
git add src/vtpty/Process_win32.cpp src/vtpty/Process.hpp src/vtpty/ConPty_test.cpp
git commit -m "feat(vtpty): start a pseudoconsole child in a held directory on Windows

The program is the .exe found on the absolute entries of the child's own
PATH, never a batch file and never one planted in the directory; the
directory is entered by its resolved path, checked against the held
handle before and after the suspended CreateProcessW; the child joins a
kill-on-close job and inherits nothing but its console. No fallback: in
its directory or not at all.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: The job's lifetime — terminate, console close, the daemon's death, a live upgrade

**Files:**
- Modify: `src/vtpty/AdoptedChild.hpp` (`CrossingHandle job {}` after `processHandle`, Doxygen: "On Windows, the
  Job Object a held-directory child runs in, crossing with it; none for a user's shell and elsewhere.")
- Modify: `src/vtpty/Process_win32.cpp` (`handoverChild`, `releaseChild`, `adopt`, `~Process`;
  `ConsoleCloseGrace`; the deferred close)
- Modify: `src/vthost/handover/HandoverCodec.cpp` (`visitor(7, child.job)`),
  `src/vthost/handover/HandoverRestore.cpp` (`handlesNamedBy` pushes `session.child.job`),
  `src/vthost/handover/HandoverImage.hpp` (Doxygen of the field list, if it enumerates them)
- Test: `src/vtpty/ConPty_test.cpp`, `src/vthost/handover/HandoverCodec_test.cpp`,
  `src/vthost/handover/SuccessorLaunch_test.cpp`

**Interfaces / rules:**

```cpp
/// How long a held-directory child's tree outlives its closed console before its job is closed: the time
/// Windows gives a console process to handle CTRL_CLOSE_EVENT.
constexpr auto ConsoleCloseGrace = std::chrono::seconds { 5 };

/// Closes @p job once @p process has exited or @p grace has passed, whichever is first, on the thread pool
/// (CreateThreadpoolWait; the callback closes the job and then its own wait object). Owns both handles.
void closeJobAfter(UniqueHandle job, UniqueHandle process, std::chrono::milliseconds grace);
```

- `terminate()` with a job terminates the job **even when the leader has already exited** (today's early
  `if (!alive()) return;` applies only to a job-less child): stragglers of an exited agent are still its tree.
- `~Process`: a held child still running and its pseudoconsole closed → `closeJobAfter(job, duplicate of the
  process handle, ConsoleCloseGrace)`; otherwise the job closes with `Private` (ending any straggler at once).
- `handoverChild()` reports the job (`CrossingHandle::of`); `releaseChild()` gives it up with the process handle,
  so the releasing `Process` never closes it. `adopt()` takes `child.job` when present and makes it
  non-inheritable (`setInheritance(…, NotInherited)`), as it does the process handle.
- The codec's field 7 is optional: an image without it decodes with `job` none (the existing "an unknown field is
  skipped and a missing one is defaulted" behaviour); no format-version change. If a golden *encoded* fixture
  exists, regenerate it and say so in the commit body.

- [ ] **Step 1: Write the failing tests.** A test helper `isDead(DWORD pid, 10s)` as phase B's (a failed open counts
  as dead); `heldProbe(...)` starts `process_runner_probe` copied into `bin` on a `ConPty` in a held `work`:

```cpp
TEST_CASE("ConPty.terminatingAHeldDirectoryChildEndsItsTree", "[vtpty][conpty][held]")
{   // heldProbe {"spawn-detached", grand, "pid", leaderFile, "sleep", "600000"}; await both pids;
    // process.terminate(Normal) → isDead(leader) && isDead(grandchild)
}
TEST_CASE("ConPty.closingAHeldDirectoryChildsConsoleEndsADetachedGrandchild", "[vtpty][conpty][held]")
{   // as above; process.close(); process.reset() returns at once (no wait for the child);
    // isDead(grandchild) within ConsoleCloseGrace + 5 s
}
TEST_CASE("ConPty.theLastHandleOfTheJobEndsTheTree", "[vtpty][conpty][held]")
{   // as above; child = releaseChild(); child->job not none and not inheritable (GetHandleInformation);
    // grandchild alive; CloseHandle(child->job) → isDead(grandchild)   // the daemon dying closes every handle
}
TEST_CASE("ConPty.aUsersShellRunsInNoJob", "[vtpty][conpty]")
{   // a spawn without a directory (probe by absolute path) → handoverChild()->job.isNone()
}
TEST_CASE("ConPty.anAdoptedHeldDirectoryChildKeepsItsJob", "[vtpty][conpty][held][handover]")
{   // heldProbe as above; child = releaseChild(); destroy the original Process (its console closes; the
    // detached grandchild must survive: isAlive(grandchild) after 2 s — the job did not close);
    // adopted = Process::adopt(*child, std::make_unique<vtpty::MockPty>(pageSize(80, 24)));
    // adopted->terminate(Normal) → isDead(grandchild)
}
```

```cpp
// HandoverCodec_test.cpp
TEST_CASE("a child's job crosses beside its process handle", "[handover]")
{   // session.child.processHandle = CrossingHandle { 60 }, .job = CrossingHandle { 64 } → encode/decode →
    // equal; a child with job none decodes with job none
}
// SuccessorLaunch_test.cpp — "an image names each of its handles once, for the successor to inherit":
//   the first session's child gains .job = CrossingHandle { 64 }; the expected list gains CrossingHandle { 64 }
```

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build --preset clangcl-release --target vtpty_test vthost_test`
Expected: compile error, `no member named 'job' in 'vtpty::AdoptedChild'`.

- [ ] **Step 3: Implement** the field, the codec row, `handlesNamedBy`, the release/adopt hand-off,
  `closeJobAfter`, `~Process`'s rule. Document at `closeJobAfter` why it may outlive the `Process` and that process
  exit closes the job anyway.

- [ ] **Step 4: Run to verify they pass**

Run: `cmake --build --preset clangcl-release --target vtpty_test vthost_test && out/build/clangcl-release/bin/vtpty_test.exe "[held]" && out/build/clangcl-release/bin/vthost_test.exe "[handover]"`
Expected: `All tests passed` for both. Repeat `vtpty_test.exe "[held]"` 10 times → 10 × `All tests passed`. Then
`out/build/clangcl-release/bin/vtpty_test.exe "[handover]"` (the existing ConPty adoption case) → passes.

- [ ] **Step 5: Commit**

```bash
git add src/vtpty/AdoptedChild.hpp src/vtpty/Process_win32.cpp src/vtpty/ConPty_test.cpp src/vthost/handover/HandoverCodec.cpp src/vthost/handover/HandoverRestore.cpp src/vthost/handover/HandoverImage.hpp src/vthost/handover/HandoverCodec_test.cpp src/vthost/handover/SuccessorLaunch_test.cpp
git commit -m "feat(vtpty): an agent's process tree ends with its pane, and survives an upgrade

A held-directory child's job is terminated with it, closed after the
console-close grace once its console is closed, and closed by the daemon
dying; a live upgrade carries the job handle beside the process handle,
so the old daemon's exit does not end the agents it hands over.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 5: `pane-open` is supported on Windows

**Files:**
- Modify: `src/vtpty/Process_win32.cpp` (`heldDirectorySpawn` → `Supported`; `findsProgram` for an `ExecInfo`
  with a directory → Decision 3's lookup)
- Modify: `src/vtpty/Process.hpp` (Doxygen: `HeldDirectorySpawn::Supported` — "POSIX: the trampoline enters the
  handle; Windows: entered by its verified path"; `heldDirectorySpawn()` — drop "nor on Windows"),
  `src/vthost/SessionHost.hpp:1262` (the comment's "(a Flatpak whose shells run on the host, Windows)" loses
  "Windows")
- Test: `src/vtpty/Process_test.cpp` (a `#ifdef _WIN32` block), `src/vthost/ShellPtyFactory_test.cpp` (a
  `#ifdef _WIN32` block)

- [ ] **Step 1: Write the failing tests.**

```cpp
// Process_test.cpp, #ifdef _WIN32
TEST_CASE("Process.a held directory can be entered on Windows", "[process]")
{
    CHECK(vtpty::Process::heldDirectorySpawn(vtpty::SandboxEscape::Contained) == vtpty::HeldDirectorySpawn::Supported);
    CHECK(vtpty::Process::heldDirectorySpawn(vtpty::SandboxEscape::Escapes) == vtpty::HeldDirectorySpawn::Supported);
}
TEST_CASE("Process.findsProgram in a held directory finds only an .exe on an absolute PATH entry", "[process]")
{   // bin\tool.exe, bin\script.cmd, work\planted.exe; exec.directory = held work, env Path = <bin>:
    // "tool" → true; "script" and "script.cmd" → false; "planted" → false; Path ".;<bin>" with "planted" →
    // false; without a directory, "script.cmd" keeps today's answer (unchanged path)
}
```

`ShellPtyFactory_test.cpp`, `#ifdef _WIN32` (helpers mirroring the POSIX ones; `SpawningHost` built with
`makeShellPtyFactory(execInfo("cmd.exe"), …)` on Windows): the agent is `VTHOST_PROCESS_RUNNER_PROBE` copied into
a `bin`, run as `probe report <report> cwd env`, `removedEnvironment = { "GIT_DIR", "GH_REPO" }`, `env = { {"Path",
bin} }`, `directory` held.

```cpp
TEST_CASE("a host without profiles spawns an agent in the handle's directory, on Windows", "[vthost][spawn]")
{   // InheritedRepositorySelection set; createTab({ agentOverride, Required }) → awaitFile(report) first line ==
    // held->resolvedPath(); no "GIT_DIR=" or "GH_REPO=" line; "GIT_SSH_COMMAND=ssh -i kept" present
}
TEST_CASE("a held directory replaced at its name starts no agent, on Windows", "[vthost][spawn]")
{   // open worktree; rename → moved; create a new worktree; createTab(...) → awaitFile(report, 3 s) is empty
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build --preset clangcl-release --target vtpty_test vthost_test && out/build/clangcl-release/bin/vtpty_test.exe "Process.*held*" && out/build/clangcl-release/bin/vthost_test.exe "[spawn]"`
Expected: the `heldDirectorySpawn` checks fail (`Unsupported`); the `findsProgram` case fails on `"planted"`
(today's lookup is not the held one); the ShellPtyFactory cases already pass if Task 3 is in — note it, they are
the end-to-end regression.

- [ ] **Step 3: Implement** the two functions and the comments.

- [ ] **Step 4: Run to verify they pass**

Run: `cmake --build --preset clangcl-release --target vtpty_test vthost_test && out/build/clangcl-release/bin/vtpty_test.exe && out/build/clangcl-release/bin/vthost_test.exe "[panes]" && out/build/clangcl-release/bin/vthost_test.exe "[spawn]"`
Expected: `vtpty_test` `All tests passed`; `[panes]` and `[spawn]`: no case fails that did not fail at the roadmap's
recorded baseline (the `[panes]` cases inject their own `heldDirectorySpawn` rule, so none changes).

- [ ] **Step 5: Commit**

```bash
git add src/vtpty/Process_win32.cpp src/vtpty/Process.hpp src/vtpty/Process_test.cpp src/vthost/SessionHost.hpp src/vthost/ShellPtyFactory_test.cpp
git commit -m "feat(vtpty): pane-open on Windows

Process::heldDirectorySpawn() now answers Supported on Windows, so an
extension's agent pane opens in its worktree; findsProgram() looks a
held-directory program up as its spawn will, so a missing or batch-file
program is refused by name before any tab opens.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

## Out of scope — gaps to raise

- **`Process::workingDirectory()` answers `"."` on Windows** (`Process_win32.cpp`, a `TODO`): a pane's reported
  cwd is empty for agents as for shells. For a held child it could answer the entry path; separate change.
- **A worktree cannot be removed while its agent runs on Windows**: every Windows process holds a handle to its
  current directory, so `git worktree remove` fails until the pane is closed. The orchestrator should close the
  pane first; phase E's end-to-end run should confirm its order.
- **The inherited `=C:`-style drive directories** reach the agent from the daemon's environment (as they reach any
  Windows shell today); they name the daemon's per-drive directories, not the worktree. Harmless for `claude`;
  worth dropping for held spawns if a tool is found to read them.
- **A graceful end for `claude` on `terminate()`**: Windows has no hangup; a console-close-first `terminate()`
  (close, then the job after the grace) would let it save state. Today `terminate()` is immediate, as before.

## Docs for phase E

- `docs/extensions.md`, panes: "On Windows an agent pane's program must be an `.exe` found on an absolute entry of
  its `PATH` (Claude Code's native installer provides `claude.exe`; npm's `claude.cmd` is refused, as every batch
  file is). It starts in its worktree by path, checked against the directory the extension was granted before and
  after the process is created: a worktree renamed or replaced since is refused rather than entered. A worktree
  path longer than 258 characters cannot be a program's directory on Windows and is refused. The agent and every
  process it starts run in one job: closing the pane, or the daemon ending, ends them all — including a browser
  the agent opened, if it was not already running. They survive a live upgrade."
- `docs/extensions.md`, Limitations: drop "pane-open is not supported on Windows" (when phase E drops the Windows
  limitation as a whole). Note: "On Windows a worktree cannot be removed while its agent pane is open."
- Roadmap table row D: "done" (phase E edits the roadmap; this plan does not).

## After the last task

- Run `/simplify` over the branch diff (AGENT.md workflow), then `clang-format` over touched files, and rebuild
  `vtpty_test vthost_test`.
- Report in the change summary: performance — none on the terminal path; a held spawn adds two path opens, one job
  and one thread-pool wait per agent. Risk — Windows-only spawn code behind an unchanged `Process` interface; one
  optional handover field (`AdoptedChild::job`), backward compatible both ways; user shells untouched. Coverage —
  `Win32HeldDirectory_test.cpp`, the `[held]` ConPty cases, the handover codec case, the Windows ShellPtyFactory
  cases; the pure limits on every platform.
