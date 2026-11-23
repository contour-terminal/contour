# Windows Process Runner Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `vtpty::PipedProcessRunner` (and so `ThreadedProcessRunner`) runs processes on Windows with the
POSIX runner's contract — argv, PATH lookup, a held working directory, an environment taken only from the
spec, both streams captured under a cap, deadlines read from the injected clock, the whole process tree ended
on timeout, cancel and destruction — so that the daemon's declared commands (`git`, `gh`, `glab`, `acli`,
`claude`, `contour agent …`) run on Windows.

**Architecture:** One Job Object per process (`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`, the handle never
inheritable): `CreateProcessW` suspended with an absolute `lpApplicationName`, assigned to the job, resumed. The
child inherits exactly three handles — `NUL` for stdin and the write ends of two named pipes — through
`PROC_THREAD_ATTRIBUTE_HANDLE_LIST`. The runner owns one I/O completion port: the read ends of the pipes are
overlapped and associated with it, `wake()` posts a packet to it, and each leader's exit posts one through a
thread-pool wait (`RegisterWaitForSingleObject`). `pump()` is one `GetQueuedCompletionStatusEx` bounded by
`maxWait` and the next clock deadline — the same round the POSIX runner does with `poll()`. Every decision that
is a string rule (which file a program name runs, which PATH entries count, which environment the child gets)
is a pure unit compiled and tested on every platform.

**Tech Stack:** C++23, Win32 (`CreateProcessW`, Job Objects, named pipes, I/O completion ports,
`RegisterWaitForSingleObject`), existing `vtpty` pieces (`buildWindowsCommandLine`, `windowsEnvironmentBlock`,
`AttributeList::inheritOnly`, `UniqueHandle`, `DirectoryHandle::nativeHandle()`/`resolvedPath()`/`isRemoved()`),
Catch2 (`vtpty_test`).

**Spec:** `docs/superpowers/plans/2026-11-23-orchestrator-windows-glab-acli-roadmap.md` (phase 3, "B — Windows
process runner"). Builds on `2026-11-23-orchestrator-windows-directory-handles.md` (phase A).

## Facts measured for this plan (2026-11-23, this machine)

- `where glab acli git gh claude`:
  `C:\ProgramData\chocolatey\bin\glab.exe`, `C:\Users\<user>\.local\bin\acli.exe`,
  `C:\Program Files\Git\mingw64\bin\git.exe` and `C:\Program Files\Git\cmd\git.exe`,
  `C:\Program Files\GitHub CLI\gh.exe`, `C:\Users\<user>\.local\bin\claude.exe`.
  **All five are `.exe`**: the `.exe`-only rule refuses none of them. `claude.exe` is the native installer's
  binary, not npm's `claude.cmd` shim.
- `C:\ProgramData\chocolatey\bin\glab.exe` is a Chocolatey *shim*: an `.exe` that starts the real `glab.exe` as
  its own child and relays its exit code. The Job Object is what makes ending it end `glab` too; a runner that
  killed only the leader would leave the real `glab` running and holding the pipes.
- The only consumer is the daemon: `ContourApp.cpp`'s `OnFirstStartProcessRunner` makes a
  `ThreadedProcessRunner` on the first declared command, and `vtextension::DeclaredProcesses` builds each
  `ProcessSpec` (argv after `resolveProgram()` maps `contour` to the daemon's own absolute executable;
  environment from `childEnvironment()`, the daemon's own less `InheritanceRules`, then the manifest's layers;
  bare program names `git`, `gh`, `glab`, `acli`, `claude` — no manifest command names a relative path).
- The runner is pumped by its own thread (`ThreadedProcessRunner`), not by the daemon's `core::net` loop.
  `core::net`'s IOCP backend (`vendor/core-cpp/src/core/net/ICompletionPort.hpp`) is a socket seam that keeps
  the association record to itself; nothing in it waits on process or pipe handles, and it is not to be edited.
  The runner therefore owns a completion port of its own.
- `src/vtpty/CommandCapture_win32.cpp` already spawns suspended into a kill-on-close job with a handle list; it
  is the model for the spawn half. `ConPty.cpp`'s `createPipe()` already makes a named pipe whose our-end is
  overlapped and whose other end is synchronous; Task 2 extracts it rather than copying it.

## Global Constraints

- Project rules in `AGENT.md`: C++23, `std::expected`, `enum class` over `bool`, Doxygen on every new public
  declaration, `.hpp` headers with `#pragma once`, no C-style loops (`std::views::iota`), no `NOLINT`, zero
  warnings under `-Werror` and clang-tidy. Data-driven: error mappings and the probe's verbs are tables.
- Never edit `vendor/core-cpp`.
- The public header `ProcessRunner.hpp` keeps its shape: no type, member or signature changes. Only Doxygen text
  gains the Windows behaviour (Task 4). POSIX code (`ProcessRunner_unix.cpp`, the `#ifndef _WIN32` half of
  `ProcessRunner_test.cpp`) is not edited; Linux CI is the gate for it.
- **No argument ever passes through `cmd.exe`** or any other interpreter the runner picks: only a file whose
  name ends in `.exe` (ASCII case-insensitive) is started, always by an absolute `lpApplicationName`.
- **Every handle the runner makes is non-inheritable**, except the child's three standard handles, which are
  made inheritable immediately before `CreateProcessW` and closed in this process immediately after it.
- Windows strings are UTF-8 at every `vtpty` interface; UTF-16 only at the Win32 call (`core::toUtf16`).
- Build: `cmake --build --preset clangcl-release --target vtpty_test` (binary:
  `out/build/clangcl-release/bin/vtpty_test.exe`). Builds take minutes: use a 10-minute timeout. Do not run two
  builds in the same tree at once.

## Decisions

1. **One Job Object per process, never `TerminateProcess` on the leader alone.** Cancel and timeout end the tree
   (`TerminateJobObject`); closing the job handle (destruction, or the daemon dying in any way) ends it too,
   because the handle is held only by the runner and never inherited. No `JOB_OBJECT_LIMIT_BREAKAWAY_OK`: a
   grandchild cannot leave the job. Nested jobs (Windows 8+) make this work when the daemon itself runs inside a
   job (a terminal, an IDE, CI).
2. **No graceful phase on Windows: termination is immediate, and `ProcessRunnerTuning::terminateGrace` is not
   used there.** There is no SIGTERM. `GenerateConsoleCtrlEvent` reaches only processes sharing the *caller's*
   console, which a detached daemon does not have, and attaching the daemon to each child's console is
   process-global and racy. Consequence, documented in the header: a `git` killed on timeout can leave
   `.git/index.lock` behind, which on POSIX its SIGTERM handler removes. A graceful Ctrl-Break through a helper
   process is a possible follow-up, not this phase.
3. **Exit mapping.** `Exited` with `exitCode = static_cast<int32_t>(GetExitCodeProcess())` for a leader that
   ended by itself (an NTSTATUS such as `0xC0000005` arrives negative). `TimedOut`/`Cancelled` with neither
   `exitCode` nor `signal` when the runner ended it — unless the leader had already exited by itself, in which
   case its `exitCode` is kept (the POSIX "unless it exited by itself first"). `signal` is never set on Windows.
   `Signalled` and `StatusLost` never occur: no one else can reap a process whose handle the runner holds. The
   exit is decided by the process handle being signalled, never by `STILL_ACTIVE` (259), which is a legal exit
   code.
4. **Program lookup is explicit and `.exe`-only.** `CreateProcessW` with a null `lpApplicationName` searches the
   application's directory, the current directory, the system directories and then PATH, and with a `.bat` or
   `.cmd` it runs `cmd.exe /c`. The runner never lets it search: a bare name gains `.exe` if it has no extension
   and is looked up on the spec's PATH's **absolute** entries only (empty, `.`, relative, drive-relative `C:x`,
   root-relative `\x` and quoted entries skipped); any extension other than `.exe` is `ProgramNotFound`.
   `PATHEXT` is ignored. Where the spec has no PATH, only an absolute program runs (no Windows default search
   path, as POSIX falls back to `confstr`).
5. **A relative program path that names a directory (`.\tool.exe`, `bin\tool.exe`) is refused
   (`ProgramNotFound`) on Windows.** POSIX resolves it in the child after `fchdir()` to the held handle; Windows
   can only hand `CreateProcessW` a path, which a directory swapped in under the same name could redirect to a
   planted binary. No manifest command uses one. This is a deliberate divergence, tested.
6. **argv[0] in the command line is the name as given** (`git`), as `posix_spawn` receives it; the absolute path
   found goes to `lpApplicationName`. The command line is `buildWindowsCommandLine(argv[0], argv[1..])`; one
   longer than 32 766 UTF-16 units is refused `SpawnFailed` before `CreateProcessW` is called.
7. **Working directory: the held handle's `resolvedPath()`, checked against the handle before and after
   `CreateProcessW`.** `CreateProcessW` takes a path, not a handle, so the POSIX `fchdir` guarantee cannot be
   had. Before the call and again after it (the child still suspended), the path is opened
   (`FILE_READ_ATTRIBUTES`, `FILE_FLAG_BACKUP_SEMANTICS`) and its `FILE_ID_INFO` compared with the held
   handle's; a mismatch terminates the suspended child and answers `DirectoryMissing`. **Residual TOCTOU,
   documented at the code:** a swap of the name to another directory and back again, both within the
   `CreateProcessW` call, goes unseen. An unbacked handle is `SpawnFailed`; `isRemoved()` is `DirectoryMissing`,
   as on POSIX.
8. **Environment.** The spec's pairs are collapsed case-insensitively (ASCII fold; the *last* of names that
   differ only in case wins, which is `childEnvironment()`'s layering intent), and the PATH the lookup searches
   is read from the collapsed list, so the PATH searched is the PATH the child sees. The block is
   `windowsEnvironmentBlock()`'s; an empty environment is still handed over as a valid block (at least two
   NULs), never as `nullptr` (which would mean "inherit the daemon's").
9. **Event plumbing: one completion port per runner.** Pipes' overlapped reads and `wake()` complete on it; each
   leader's exit is posted to it by `RegisterWaitForSingleObject(…, WT_EXECUTEONLYONCE)`. Rejected:
   `WaitForMultipleObjects` (64 handles: about 20 concurrent processes), job-object completion messages (their
   delivery is documented as not guaranteed), a 20 ms polling slice (wakes the daemon 50 times a second for the
   whole life of a minutes-long `claude` command). A completion packet identifies its process by completion key
   = the token's value and its stream by comparing the `OVERLAPPED*` with that process's two; a packet whose
   token has completed is ignored and never dereferenced.
10. **Creation flags:** `CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT |
    EXTENDED_STARTUPINFO_PRESENT`. `CREATE_NO_WINDOW` gives console programs a console that is never shown (a
    detached daemon would otherwise flash a window per command). stdin is `NUL`, as POSIX's `/dev/null`.
11. **`npm`-installed `claude` (`claude.cmd`) is refused `ProgramNotFound`**, by Decision 4. The native installer
    (what this machine has) ships `claude.exe`. `docs/extensions.md` says so in Task 4; mapping `claude` to
    `node.exe …\cli.js` through the program table (`SelfPrograms`) is left for later if users ask.

## Out of scope — gaps to raise

- **`vthost/StdioChild_win32.cpp` creates its pipes inheritable and calls `CreateProcessW(…, TRUE, …)` without a
  handle list.** It inherits every inheritable handle in the daemon, so an `ssh` it starts during the instant the
  runner's child ends are inheritable (Constraint above) would hold a runner pipe open. The runner bounds the
  damage (the orphaned-output grace closes our end), but the fix is in `StdioChild_win32.cpp`: a handle list,
  as `CommandCapture_win32.cpp` has. Separate change.
- **`childEnvironment()` compares names case-sensitively**, so on Windows a daemon `Git_Dir` would slip past the
  `GIT_` withholding rule. Raise against `vtextension`; this phase only collapses duplicates (Decision 8).
- Phase A's gap stands: `vtextension`'s root grammar refuses `C:\…`, so `@repos`/`@worktrees` commands do not
  reach the runner on Windows yet.

## Review Focus

- **No `cmd.exe`, ever.** A `.cmd`/`.bat`/`.com`/`.ps1` named bare or by absolute path, or found on PATH, is
  `ProgramNotFound` and nothing runs (a marker the batch file would write stays absent); arguments holding
  `&`, `|`, `%PATH%`, `^`, `"` arrive byte-exact. Task 1 (rules), Task 3 ("a batch file is never run",
  "arguments reach the child exactly").
- **No handle leaks into children.** Only `NUL` and the two pipe write ends are in the handle list; a handle the
  daemon made inheritable for its own reasons does not reach the child; one child's pipe ends do not reach a
  second child started meanwhile. Task 4 ("an inheritable handle of the daemon does not reach the child"),
  Task 3 ("two processes complete independently" with the orphan grace never reached).
- **Children die with the daemon.** The job handle is non-inheritable and kill-on-close; a runner host that
  dies by `TerminateProcess` without destructors takes its grandchild along. Task 4.
- **PATH entries that are empty, `.`, relative, drive-relative or quoted are skipped; the current directory and
  the application's directory are never searched.** `lpApplicationName` is always absolute. Task 1, Task 3
  ("a program planted in the directory is never found on the PATH", "the test binary's own directory is not
  searched").
- **Long output:** 2 MiB past a 1 MiB cap is cut and drained; 64 MiB on a discarded stream leaves it empty and
  `Truncated`; a writer that never pauses cannot starve deadlines (bounded reads per round). Task 3.
- **A child that closes stdout but keeps running** completes when its leader exits, not at EOF. Task 3.
- **A grandchild that holds the pipe** after its leader exited: after `orphanedOutputGrace` by the clock the job
  is terminated and the process completes `Exited` with the leader's code and what was read. Task 4.
- **Overlapped I/O lifetime:** no `OVERLAPPED` or buffer is freed while its read is pending — a closed stream is
  `CancelIoEx`ed and `GetOverlappedResult(…, TRUE)` awaited first; `UnregisterWaitEx(…, INVALID_HANDLE_VALUE)`
  precedes closing a process handle; the destructor unregisters every wait before closing the port.
- **Exit code 259 and NTSTATUS codes** (Decision 3). Task 3.
- **Working directory**: the probe's `cwd` equals `resolvedPath()`; a directory replaced at its name is
  `DirectoryMissing`, not entered. Task 3.

---

### Task 1: The Windows spawn rules, pure and platform-independent

**Files:**
- Create: `src/vtpty/WindowsProcessRules.hpp`, `src/vtpty/WindowsProcessRules.cpp`
- Create: `src/vtpty/WindowsProcessRules_test.cpp`
- Modify: `src/vtpty/CMakeLists.txt` (`WindowsProcessRules.cpp` in `vtpty_SOURCES` beside
  `WindowsPathRules.cpp`, the header in `vtpty_HEADERS`, the test in `vtpty_test`'s unconditional list)

**Interfaces** (namespace `vtpty`; strings UTF-8; pure string work — no `std::filesystem`, whose `is_absolute`
answers differently on Linux, so the rules test the same on every platform):

```cpp
/// The extensions a program file may have to be started: one row, so that the rule is data.
inline constexpr auto WindowsImageExtensions = std::array { std::string_view { ".exe" } };

/// @param path A path, UTF-8.
/// @return Whether @p path is absolute as Win32 reads it: `C:\…` or `C:/…`, `\\server\share…`, `\\?\…`.
///         Drive-relative `C:x`, root-relative `\x`, relative and double-quoted paths are not.
[[nodiscard]] bool isAbsoluteWindowsPath(std::string_view path) noexcept;

/// @param program A program's file name or path, UTF-8.
/// @return The file it names to start: @p program with `.exe` appended where its last component has no
///         extension; @p program itself where the extension is in WindowsImageExtensions (ASCII
///         case-insensitive); std::nullopt for any other extension (`.cmd`, `.bat`, `.com`, `.ps1`, …), a
///         trailing dot, or an empty name.
[[nodiscard]] std::optional<std::string> windowsImageName(std::string_view program);

/// Finds the file a program runs from, for a spawn into a held directory, as findHeldDirectoryProgram() does
/// on POSIX, with Windows' rules: windowsImageName() first; an absolute program is taken as it is; a relative
/// one carrying a directory (`.\x`, `bin\x`, `C:x`) is refused; a bare name is looked up on the absolute
/// entries of @p searchPath (`;`-separated) only.
/// @param program The program as declared.
/// @param searchPath The PATH it runs under.
/// @param isExecutable Whether a candidate (absolute, UTF-8) names a file to run.
/// @return The absolute path to start; std::nullopt where none is found or the program is refused.
[[nodiscard]] std::optional<std::string> findWindowsHeldDirectoryProgram(std::string_view program,
                                                                         std::string_view searchPath,
                                                                         ExecutableTest const& isExecutable);

/// @param environment A spec's environment, in order.
/// @return It as "NAME=VALUE" entries, names that differ only in ASCII case collapsed to the last of them.
[[nodiscard]] std::vector<std::string> windowsEnvironmentEntries(
    std::span<std::pair<std::string, std::string> const> environment);

/// @param environment A spec's environment, in order.
/// @return The value of its PATH, the name compared ASCII-case-insensitively, the last one winning; empty
///         where it has none.
[[nodiscard]] std::string windowsSearchPathOf(std::span<std::pair<std::string, std::string> const> environment);
```

Joining an entry and a name adds `\` only where the entry does not already end in `\` or `/`.

- [ ] **Step 1: Write the failing tests** in `WindowsProcessRules_test.cpp`, tag `[windowsprocessrules]`:

```cpp
TEST_CASE("windowsImageName appends .exe to a bare name and keeps an .exe", "[windowsprocessrules]")
{
    CHECK(vtpty::windowsImageName("git") == "git.exe");
    CHECK(vtpty::windowsImageName("git.exe") == "git.exe");
    CHECK(vtpty::windowsImageName("GIT.EXE") == "GIT.EXE");
    CHECK(vtpty::windowsImageName(R"(C:\Program Files\Git\cmd\git)") == R"(C:\Program Files\Git\cmd\git.exe)");
    CHECK(vtpty::windowsImageName(R"(C:\tools.d\x)") == R"(C:\tools.d\x.exe)"); // the dot is the directory's
}

TEST_CASE("windowsImageName refuses every other extension", "[windowsprocessrules]")
{
    for (auto const name: { "claude.cmd", "x.bat", "X.CMD", "x.com", "x.ps1", "x.vbs", "x.lnk", "x.", "",
                            R"(C:\a\b.cmd)", "x.exe.cmd" })
    {
        CAPTURE(name);
        CHECK_FALSE(vtpty::windowsImageName(name).has_value());
    }
}

TEST_CASE("isAbsoluteWindowsPath", "[windowsprocessrules]")
{
    for (auto const path: { R"(C:\x)", "c:/x", R"(C:\)", R"(\\srv\share\x)", R"(\\?\C:\x)" })
        CHECK(vtpty::isAbsoluteWindowsPath(path));
    for (auto const path: { "C:x", R"(\x)", "x", R"(.\x)", ".", "", R"("C:\x")", "/usr/bin" })
    {
        CAPTURE(path);
        CHECK_FALSE(vtpty::isAbsoluteWindowsPath(path));
    }
}

TEST_CASE("findWindowsHeldDirectoryProgram searches only absolute PATH entries", "[windowsprocessrules]")
{
    auto const files = std::set<std::string> { R"(C:\bin\tool.exe)", R"(C:\other\tool.exe)", R"(bin\tool.exe)",
                                               R"(.\tool.exe)", R"(tool.exe)", R"(C:\cmds\tool.cmd)" };
    auto const exists = [&](std::string const& candidate) { return files.contains(candidate); };
    using vtpty::findWindowsHeldDirectoryProgram;
    CHECK(findWindowsHeldDirectoryProgram("tool", R"(C:\bin;C:\other)", exists) == R"(C:\bin\tool.exe)");
    CHECK(findWindowsHeldDirectoryProgram("tool", R"(C:\bin\;C:\other)", exists) == R"(C:\bin\tool.exe)");
    for (auto const path: { R"(;C:\other)", R"(.;C:\other)", R"(bin;C:\other)", R"(C:;C:\other)",
                            R"("C:\bin";C:\other)", R"(\bin;C:\other)" })
    {
        CAPTURE(path);
        CHECK(findWindowsHeldDirectoryProgram("tool", path, exists) == R"(C:\other\tool.exe)");
    }
    CHECK_FALSE(findWindowsHeldDirectoryProgram("tool", R"(;.;bin)", exists).has_value());
    CHECK_FALSE(findWindowsHeldDirectoryProgram("tool", "", exists).has_value());
    CHECK_FALSE(findWindowsHeldDirectoryProgram("tool.cmd", R"(C:\cmds)", exists).has_value());
}

TEST_CASE("findWindowsHeldDirectoryProgram refuses a relative path with a directory", "[windowsprocessrules]")
{
    auto const always = [](std::string const&) { return true; };
    for (auto const program: { R"(.\tool)", R"(bin\tool.exe)", "bin/tool", "C:tool", R"(\tool)" })
    {
        CAPTURE(program);
        CHECK_FALSE(vtpty::findWindowsHeldDirectoryProgram(program, R"(C:\bin)", always).has_value());
    }
}

TEST_CASE("findWindowsHeldDirectoryProgram takes an absolute .exe as it is", "[windowsprocessrules]")
{
    auto const exists = [](std::string const& candidate) { return candidate == R"(C:\c\contour.exe)"; };
    CHECK(vtpty::findWindowsHeldDirectoryProgram(R"(C:\c\contour.exe)", "", exists) == R"(C:\c\contour.exe)");
    CHECK(vtpty::findWindowsHeldDirectoryProgram(R"(C:\c\contour)", "", exists) == R"(C:\c\contour.exe)");
    CHECK_FALSE(vtpty::findWindowsHeldDirectoryProgram(R"(C:\c\run.bat)", "", exists).has_value());
    CHECK_FALSE(vtpty::findWindowsHeldDirectoryProgram(R"(C:\c\missing.exe)", "", exists).has_value());
}

TEST_CASE("windowsEnvironmentEntries collapses names that differ in case, the last winning",
          "[windowsprocessrules]")
{
    auto const environment = std::vector<std::pair<std::string, std::string>> {
        { "Path", R"(C:\a)" }, { "HOME", "h" }, { "PATH", R"(C:\b)" }, { "x", "1" }, { "X", "2" }
    };
    auto entries = vtpty::windowsEnvironmentEntries(environment);
    std::ranges::sort(entries);
    CHECK(entries == std::vector<std::string> { "HOME=h", R"(PATH=C:\b)", "X=2" });
}

TEST_CASE("windowsSearchPathOf finds PATH in any case", "[windowsprocessrules]")
{
    using Environment = std::vector<std::pair<std::string, std::string>>;
    CHECK(vtpty::windowsSearchPathOf(Environment { { "Path", R"(C:\a)" } }) == R"(C:\a)");
    CHECK(vtpty::windowsSearchPathOf(Environment { { "Path", R"(C:\a)" }, { "PATH", R"(C:\b)" } }) == R"(C:\b)");
    CHECK(vtpty::windowsSearchPathOf(Environment { { "PATHEXT", ".EXE" } }).empty());
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build --preset clangcl-release --target vtpty_test`
Expected: compile error, `'vtpty/WindowsProcessRules.hpp' file not found`.

- [ ] **Step 3: Implement.** `findWindowsHeldDirectoryProgram`: `windowsImageName` first; then
  `isAbsoluteWindowsPath(program)` → `isExecutable(image)`; a program containing `\`, `/` or `:` that is not
  absolute → `std::nullopt`; else `std::views::split(searchPath, ';')` keeping `isAbsoluteWindowsPath` entries.
  The fold for names is ASCII only (document it: Windows folds Unicode, which no real variable name needs).

- [ ] **Step 4: Run to verify they pass**

Run: `cmake --build --preset clangcl-release --target vtpty_test && out/build/clangcl-release/bin/vtpty_test.exe "[windowsprocessrules]"`
Expected: `All tests passed (… assertions in 8 test cases)`.

- [ ] **Step 5: Commit**

```bash
git add src/vtpty/WindowsProcessRules.hpp src/vtpty/WindowsProcessRules.cpp src/vtpty/WindowsProcessRules_test.cpp src/vtpty/CMakeLists.txt
git commit -m "feat(vtpty): the rules a Windows spawn obeys, as a pure unit

Which file a program name starts (an .exe only, never a batch file),
which PATH entries are searched (absolute ones only), and the environment
a child gets with names compared as Windows compares them. Tested on
every platform.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 2: Shared overlapped pipe, and the test programs

**Files:**
- Create: `src/vtpty/OverlappedPipe.hpp` (whole header `#ifdef _WIN32`, includes `Win32Handle.hpp`),
  `src/vtpty/OverlappedPipe_win32.cpp`
- Modify: `src/vtpty/ConPty.cpp` (delete `PipeFlow`, `PipeFlowRow`, `PipeFlows`, `processStartTime()`,
  `createPipe()`; call `createOverlappedPipe`), `src/vtpty/UniqueHandle.hpp` (add `release()`)
- Create: `src/vtpty/test/process_runner_probe.cpp`, `src/vtpty/test/process_runner_host.cpp`
- Modify: `src/vtpty/CMakeLists.txt` (`OverlappedPipe_win32.cpp` appended in the Windows `else()` block beside
  `ConPty.cpp`, its header in `vtpty_HEADERS` there; the two programs in the `if(WIN32)` test block, as
  `gui_subsystem_echo` is, with `add_dependencies(vtpty_test …)` and
  `VTPTY_PROCESS_RUNNER_PROBE="$<TARGET_FILE:process_runner_probe>"`,
  `VTPTY_PROCESS_RUNNER_HOST="$<TARGET_FILE:process_runner_host>"`; the probe links `core::base` only, the host
  links `vtpty`)
- Test: `src/vtpty/ConPty_test.cpp` unchanged (regression), new smoke case in
  `src/vtpty/CommandCapture_test.cpp`'s `#ifdef _WIN32` part

**Interfaces:**

```cpp
// UniqueHandle.hpp
/// Gives up ownership without closing.
/// @return The handle; the UniqueHandle owns none afterwards.
[[nodiscard]] core::platform::NativeHandle release() noexcept;

// OverlappedPipe.hpp
/// Which way a pipe carries bytes, seen from this process.
enum class PipeDirection : uint8_t
{
    Inbound = 0, ///< This process reads; the other end writes.
    Outbound,    ///< This process writes; the other end reads (duplex, so that it may ask for room).
};

/// Both ends of a pipe made by createOverlappedPipe().
struct OverlappedPipe
{
    UniqueHandle ours;   ///< Opened for overlapped I/O; not inheritable.
    UniqueHandle theirs; ///< Synchronous, as a child or a pseudoconsole needs it; not inheritable.
};

/// Creates a one-instance, local-only named pipe whose end of this process is overlapped (CreatePipe() gives
/// no such end). Its name is unique across processes and time: purpose, pid, process start time, a counter.
/// @param direction Which way it carries bytes.
/// @param purpose Part of its name (`conpty-out`, `runner-stdout`, …).
/// @param bufferSize The pipe's buffer, each way.
/// @return The two ends; or why the pipe could not be made.
[[nodiscard]] std::expected<OverlappedPipe, std::error_code> createOverlappedPipe(PipeDirection direction,
                                                                                std::wstring_view purpose,
                                                                                std::uint32_t bufferSize);
```

The access rows (`PIPE_ACCESS_INBOUND` / `GENERIC_WRITE | FILE_READ_ATTRIBUTES`; `PIPE_ACCESS_DUPLEX` /
`GENERIC_READ | FILE_WRITE_ATTRIBUTES`) move with their comments into a `constexpr std::array` indexed by
`PipeDirection`, the `static_assert` on their order with them. Names are `contour-<purpose>-<pid>-<start>-<n>`;
ConPty passes `L"conpty-out"`/`L"conpty-in"`, so its names change from `contour-conpty-<pid>-<start>-<n>-out` to
`contour-conpty-out-<pid>-<start>-<n>`. Before changing them, check `grep -rn "contour-conpty" src` finds
nothing that reads the old spelling (the handover passes handles, not names); if something does, keep ConPty's
spelling.

**The probe** (`process_runner_probe.exe`, console subsystem): runs its arguments as a sequence of verbs, from
one `constexpr std::array` of `{name, arity, action}` rows; `args` and the two `spawn` verbs consume the rest:

| verb | effect |
| --- | --- |
| `out <text>` / `err <text>` | writes `<text>\n` to stdout / stderr |
| `zeros <n>` / `zeros-err <n>` | writes `n` zero bytes to stdout / stderr in 64 KiB writes |
| `exit <n>` | `ExitProcess(static_cast<UINT>(n))`, `n` decimal, may be negative |
| `sleep <ms>` | `Sleep(ms)` |
| `pid <file>` | writes its pid to `<file>` (written to `<file>.tmp`, then `MoveFileExW` — never half a pid) |
| `touch <file>` | creates `<file>` |
| `env` | writes `GetEnvironmentStringsW()`'s entries as UTF-8, one per line |
| `cwd` | writes `GetCurrentDirectoryW()` as UTF-8 and `\n` |
| `self` | writes `GetModuleFileNameW(nullptr)` as UTF-8 and `\n`: which file ran |
| `stdin` | reads stdin to EOF, writes the byte count and `\n` |
| `close-out` / `close-err` | `CloseHandle(GetStdHandle(…))` |
| `args …` | writes the remaining arguments, `\|`-separated, and `\n` |
| `spawn <file> …` | starts itself with the remaining verbs, inheriting its standard handles (bInheritHandles TRUE, STARTF_USESTDHANDLES), writes the grandchild's pid to `<file>`, does not wait |
| `spawn-quiet <file> …` | the same, the grandchild's standard handles `NUL`, nothing inherited |

At the end of the verbs it exits 0.

**The host** (`process_runner_host.exe <probe> <pidfile>`): makes a `PipedProcessRunner` on
`core::platform::defaultSteadyClock()`, starts `<probe> pid <pidfile> sleep 600000` (spec: no working
directory, environment `{PATH: <probe's directory>}`), pumps until `<pidfile>` exists (bounded, 10 s, else exit
2), then `TerminateProcess(GetCurrentProcess(), 7)` — no destructor runs. Used in Task 4.

- [ ] **Step 1: Write the failing smoke test** in `CommandCapture_test.cpp`'s Windows part (the probe is run by
  `ProgramCommandCapture`, which works on Windows today):

```cpp
TEST_CASE("process_runner_probe runs its verbs", "[processrunner][probe]")
{
    auto capture = vtpty::ProgramCommandCapture { core::async::StopToken {} };
    auto const line = capture.run(std::format(R"("{}" args a "b c")", VTPTY_PROCESS_RUNNER_PROBE), 10s, {});
    REQUIRE(line.has_value());
    CHECK(*line == "a|b c");
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build --preset clangcl-release --target vtpty_test`
Expected: compile error, `use of undeclared identifier 'VTPTY_PROCESS_RUNNER_PROBE'`.

- [ ] **Step 3: Implement** `release()`, `createOverlappedPipe`, the ConPty switch-over
  (`_output = inputPipe->ours.release()`, the pseudoconsole ends stay `UniqueHandle`s, so the explicit
  `CloseHandle` calls after `createPseudoConsole` go), the probe, the host, the CMake wiring.

- [ ] **Step 4: Run to verify**

Run: `cmake --build --preset clangcl-release --target vtpty_test && out/build/clangcl-release/bin/vtpty_test.exe "[probe]" && out/build/clangcl-release/bin/vtpty_test.exe "ConPty*"`
Expected: `All tests passed` for both: the smoke case, and every ConPty case as at baseline.

- [ ] **Step 5: Commit**

```bash
git add src/vtpty/OverlappedPipe.hpp src/vtpty/OverlappedPipe_win32.cpp src/vtpty/ConPty.cpp src/vtpty/UniqueHandle.hpp src/vtpty/test/process_runner_probe.cpp src/vtpty/test/process_runner_host.cpp src/vtpty/CommandCapture_test.cpp src/vtpty/CMakeLists.txt
git commit -m "refactor(vtpty): share ConPty's overlapped pipe; add the runner's test programs

createOverlappedPipe() is ConPty's named pipe with an overlapped end of
ours, now for the process runner too. process_runner_probe runs a
sequence of verbs (write, exit, sleep, spawn a grandchild, print its
environment and directory); process_runner_host dies holding a runner.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 3: `PipedProcessRunner` on Windows: start, pump, natural exits

**Files:**
- Modify: `src/vtpty/ProcessRunner_win32.cpp` (rewrite)
- Create: `src/vtpty/ProcessRunner_win32_test.cpp`
- Modify: `src/vtpty/ProcessRunner_test.cpp` (delete the `#else` "start is unsupported" case; the file then ends
  `#endif` after the POSIX block)
- Modify: `src/vtpty/CMakeLists.txt` (`ProcessRunner_win32_test.cpp` in the `if(WIN32)` test sources)

**Shape of `ProcessRunner_win32.cpp`** (anonymous namespace; names indicative, bodies the implementer's):

```cpp
constexpr auto ReadChunkSize = std::size_t { 64 } * 1024;   // as POSIX
constexpr auto ReadsPerStreamPerRound = 16;                 // as POSIX ReadChunksPerRound
constexpr auto PipeBufferSize = std::uint32_t { 64 } * 1024;
constexpr auto MaxCommandLine = std::size_t { 32766 };      // UTF-16 units, CreateProcessW's limit
constexpr auto WakeKey = ULONG_PTR { 0 };                   // tokens start at 1

/// What a failed CreateProcessW means for the start: one row per error, the rest SpawnFailed.
constexpr auto StartErrors = std::array {
    std::pair { DWORD { ERROR_FILE_NOT_FOUND }, ProcessStartError::ProgramNotFound },
    std::pair { DWORD { ERROR_PATH_NOT_FOUND }, ProcessStartError::ProgramNotFound },
    std::pair { DWORD { ERROR_ACCESS_DENIED }, ProcessStartError::ProgramNotFound },
    std::pair { DWORD { ERROR_BAD_EXE_FORMAT }, ProcessStartError::ProgramNotFound },
    std::pair { DWORD { ERROR_EXE_MACHINE_TYPE_MISMATCH }, ProcessStartError::ProgramNotFound },
    std::pair { DWORD { ERROR_DIRECTORY }, ProcessStartError::DirectoryMissing },
};

/// One output pipe: the read end, its pending read, and what was kept.
struct Stream
{
    UniqueHandle pipe;                       ///< Overlapped read end, on the runner's port; closed at EOF.
    std::unique_ptr<PendingRead> read;       ///< OVERLAPPED + 64 KiB buffer; stable while a read is pending.
    ReadState state = ReadState::Idle;       ///< enum class { Idle, Pending }
    std::size_t cap = 0;
    CapturedStream captured;
};

/// Whether the leader has exited, as the runner has seen it.
enum class Leader : uint8_t { Running, Exited };
/// How far the runner has gone in ending a process.
enum class Ending : uint8_t { NotAsked, Killed };

struct Child
{
    UniqueHandle job;                         ///< Kill-on-close; held only here.
    UniqueHandle process;                     ///< The leader.
    HANDLE exitWait = nullptr;                ///< RegisterWaitForSingleObject's; unregistered before `process` closes.
    std::unique_ptr<ExitPost> exitPost;       ///< {port, token}: what the wait callback posts; outlives the wait.
    std::array<Stream, 2> streams;
    SteadyTimePoint deadline;
    Ending ending = Ending::NotAsked;
    std::optional<ProcessCompletion> endedBy; ///< TimedOut or Cancelled.
    Leader leader = Leader::Running;
    std::optional<int32_t> exitCode;          ///< Read once the leader has exited by itself.
    SteadyTimePoint leaderExitedAt;
    CompletionHandler done;
};
```

`State` holds the clock, the tuning, the port (`UniqueHandle`), the mutex, `std::map<ProcessToken, Child>` and
the next token, like POSIX. `create()` answers `WakeChannelUnavailable` when
`CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1)` fails.

**`start()`**, in order (every early return leaves nothing running and no handle open):
1. `argv` empty → `ProgramNotFound`. Working directory: `nativeHandle() == nullptr` → `SpawnFailed`;
   `isRemoved()` → `DirectoryMissing`; FILE_ID check (Decision 7) → `DirectoryMissing`.
2. `entries = windowsEnvironmentEntries(spec.environment)`; program =
   `findWindowsHeldDirectoryProgram(argv[0], windowsSearchPathOf(spec.environment), isExecutable)` where
   `isExecutable` is `core::platform::NativeFileSystem::instance().isExecutableFile()` on the UTF-8 → path
   (`core::pathFromUtf8`); none → `ProgramNotFound`.
3. Command line `buildWindowsCommandLine(argv[0], argv | drop(1))` → UTF-16 (not UTF-8 → `SpawnFailed`;
   longer than `MaxCommandLine` → `SpawnFailed`); block `windowsEnvironmentBlock(entries)` (empty → `L"\0\0"`).
4. Two `createOverlappedPipe(PipeDirection::Inbound, L"runner-stdout"/L"runner-stderr", PipeBufferSize)`; `NUL`
   opened non-inheritable (`CreateFileW(L"NUL", GENERIC_READ, …)`); job created and set kill-on-close;
   `AttributeList::create(1)` with `inheritOnly({nul, outWrite, errWrite})`.
5. `setInheritance(…, Inherited)` on the three; `CreateProcessW(program, commandLine, nullptr, nullptr, TRUE,
   flags (Decision 10), block, resolvedPath or nullptr, &startup, &info)`; the three child-side handles closed
   right after, success or not. Failure → `StartErrors` row.
6. FILE_ID check again (child suspended); then `AssignProcessToJobObject`, `ResumeThread`; any failure →
   `TerminateProcess(process, 1)` and `SpawnFailed`.
7. Associate both read ends with the port (key = the token value, token taken under the lock first);
   `RegisterWaitForSingleObject(&exitWait, process, onLeaderExit, exitPost.get(), INFINITE,
   WT_EXECUTEONLYONCE)`; insert the `Child`; `wake()`.

**`pump(maxWait)`**: under the lock, apply deadlines (Task 4 fills in timeout/orphan; here only "leader exited
and both streams closed → completed"), issue a `ReadFile` on every open `Idle` stream (immediate
`ERROR_BROKEN_PIPE` → close; `ERROR_IO_PENDING` or success → `Pending`, its packet comes through the port),
compute the wait as POSIX does (`maxWait`, the next deadline, `0` if something is complete). Unlocked:
`GetQueuedCompletionStatusEx(port, entries[64], …, wait, FALSE)`. Locked again: for each entry — `WakeKey` →
nothing; an exit post → mark `Leader::Exited`, read `GetExitCodeProcess`, stamp `leaderExitedAt` by the clock;
a stream packet → `GetOverlappedResult(pipe, &read->overlapped, &n, FALSE)`: bytes kept to the cap (rest
dropped, `Truncated`), `ERROR_BROKEN_PIPE`/0 bytes → close the stream; then drain further packets with a zero
timeout up to `ReadsPerStreamPerRound` per stream. Then take the completed children (each: `UnregisterWaitEx(…,
INVALID_HANDLE_VALUE)`, job closed — which ends any straggler) and call their handlers unlocked, as POSIX does.

**Tests** — `ProcessRunner_win32_test.cpp`, whole file `#ifdef _WIN32`, tag `[processrunner]`. Its
`RunnerFixture` mirrors the POSIX one: `ScopedTempDir directory`, `ManualClock clock`, `tuning`, `runner`,
`finished`, `start()`, `pumpUntil()`, `runToCompletion()`, `run()`; plus `bin = directory / "bin"` holding a
copy of `VTPTY_PROCESS_RUNNER_PROBE` named `probe.exe`, and `work = directory / "work"`. `spec(argv, timeout =
30s)` opens `work` as the working directory, environment `{ { "PATH", bin.string() } }`, cap 1 MiB. Ported test
names are the POSIX ones verbatim so the two suites read side by side.

```cpp
TEST_CASE("PipedProcessRunner: exit codes", "[processrunner]")
{   // run({"probe","exit","0"}) → Exited, exitCode 0, no signal; {"probe","exit","3"} → 3; running() == 0
}
TEST_CASE("PipedProcessRunner: both streams", "[processrunner]")
{   // {"probe","out","out","err","err"} → stdout "out\n" Whole, stderr "err\n" Whole
}
TEST_CASE("PipedProcessRunner: output past the cap is drained and cut", "[processrunner]")
{   // {"probe","zeros","2097152"} → Exited 0, stdout size == 1 MiB, Truncated; stderr Whole
}
TEST_CASE("PipedProcessRunner: a discarded stream stays empty however much is written", "[processrunner]")
{   // {"probe","zeros-err","67108864","out","done"}, standardError Discard, 5000 rounds →
    // Exited 0, stdout "done\n", stderr empty and Truncated
}
TEST_CASE("PipedProcessRunner: the child sees exactly the given environment", "[processrunner]")
{   // ScopedEnv CONTOUR_PROCESSRUNNER_TEST_LEAK set in the test process; spec env + ONLY_GIVEN=yes,
    // WITH_SPACE="a b"; {"probe","env"} → sorted lines == sorted "NAME=VALUE" of the spec, nothing else
}
TEST_CASE("PipedProcessRunner: an empty environment still starts", "[processrunner]")
{   // spec({ abs probe path, "env" }) with environment {} → Exited 0, stdout empty
}
TEST_CASE("PipedProcessRunner: PATH is found whatever its case", "[processrunner]")
{   // environment { {"Path", bin} } → {"probe","exit","5"} exits 5
}
TEST_CASE("PipedProcessRunner: a program that is not installed", "[processrunner]")
{   // {"contour-no-such-program-here"} → ProgramNotFound, handler never called, running() == 0
}
TEST_CASE("PipedProcessRunner: a program planted in the directory is never found on the PATH", "[processrunner]")
{   // probe copies planted as work\probe.exe, work\bin\probe.exe, work\only-planted.exe;
    // PATH = GENERATE(";<bin>", "<bin>;;", "bin;<bin>", ".;<bin>", "C:;<bin>");
    // {"probe","self"} → stdout == "<bin>\probe.exe\n" (the real one ran, no planted copy);
    // the absolute path of bin\probe.exe runs whatever PATH; {"only-planted"} → ProgramNotFound
}
TEST_CASE("PipedProcessRunner: the test binary's own directory is not searched", "[processrunner]")
{   // {"vtpty_test"} with PATH = <bin> → ProgramNotFound (CreateProcess's own search would find it)
}
TEST_CASE("PipedProcessRunner: a batch file is never run", "[processrunner]")
{   // bin\tool.cmd and bin\tool.bat containing `echo ran > "<marker>"`; {"tool.cmd"}, {"tool.bat"},
    // {"<bin>\tool.cmd"} → each ProgramNotFound; {"tool"} with only tool.cmd present → ProgramNotFound;
    // marker never exists
}
TEST_CASE("PipedProcessRunner: a relative program path is refused", "[processrunner]")
{   // work\probe.exe present; {".\\probe.exe","exit","0"} and {"bin\\probe.exe"} → ProgramNotFound
    // (Decision 5: the POSIX "found in the directory entered by handle" does not port)
}
TEST_CASE("PipedProcessRunner: arguments reach the child exactly", "[processrunner]")
{   // {"probe","args","a b","","q\"uote","back\\\\","&","|","%PATH%","^x","tail\\"} →
    // stdout == "a b||q\"uote|back\\\\|&|||%PATH%|^x|tail\\\n"  (empty argument between `a b` and the quote;
    // `|` the separator and `|` an argument, as the probe joins them)
}
TEST_CASE("PipedProcessRunner: an over-long command line is refused before it is started", "[processrunner]")
{   // {"probe","args", std::string(33000, 'x')} → SpawnFailed
}
TEST_CASE("PipedProcessRunner: the child runs in the held directory", "[processrunner]")
{   // {"probe","cwd"} → stdout == opened.resolvedPath() + "\n"
}
TEST_CASE("PipedProcessRunner: a removed working directory is refused", "[processrunner]")
{   // open work\gone, remove it, start → DirectoryMissing
}
TEST_CASE("PipedProcessRunner: a directory replaced at its name is refused, not entered", "[processrunner]")
{   // open work\a; rename a → a.old; create a new work\a; start {"probe","cwd"} with the old handle →
    // DirectoryMissing
}
TEST_CASE("PipedProcessRunner: an unbacked working directory is refused", "[processrunner]")
{   // workingDirectory = DirectoryHandle::unbacked(work) → SpawnFailed
}
TEST_CASE("PipedProcessRunner: stdin is empty", "[processrunner]")
{   // {"probe","stdin"} → "0\n"
}
TEST_CASE("PipedProcessRunner: a crash status arrives as a negative exit code", "[processrunner]")
{   // {"probe","exit","-1073741819"} → Exited, exitCode == static_cast<int32_t>(0xC0000005)
}
TEST_CASE("PipedProcessRunner: exit code 259 is an exit, not a running process", "[processrunner]")
{   // {"probe","exit","259"} → Exited, exitCode 259
}
TEST_CASE("PipedProcessRunner: a child that closes its streams completes when it exits", "[processrunner]")
{   // {"probe","out","a","close-out","close-err","sleep","300","exit","4"} → Exited 4, stdout "a\n"
}
TEST_CASE("PipedProcessRunner: a GUI-subsystem program's output is captured", "[processrunner]")
{   // abs VTPTY_GUI_SUBSYSTEM_ECHO with args {"x","y z"} → stdout "x|y z\n"
}
TEST_CASE("PipedProcessRunner: two processes complete independently", "[processrunner]")
{   // tuning.orphanedOutputGrace = 1h, clock never advanced; one = {"probe","exit","1"},
    // two = {"probe","sleep","2000","exit","2"}; one != two; running() == 2; one completes with 1 while two
    // still runs (so two did not inherit one's pipe); then two with 2; running() == 0
}
TEST_CASE("PipedProcessRunner: no handler runs inside start", "[processrunner]")
{   // start → finished empty; then completes Exited 0
}
TEST_CASE("ThreadedProcessRunner: runs to completion", "[processrunner][threading]")
{   // as POSIX, argv {"probe","out","x"} with PATH = the probe's copy directory → Exited, stdout "x\n"
}
```

- [ ] **Step 1: Write the tests above** (bodies spelled out from the comments; `CHECK`s exactly as stated), and
  delete the "start is unsupported" case from `ProcessRunner_test.cpp`.

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build --preset clangcl-release --target vtpty_test && out/build/clangcl-release/bin/vtpty_test.exe "[processrunner]"`
Expected: builds; the cases fail at their first `REQUIRE(token.has_value())` with `Unsupported` (the probe
smoke case from Task 2 still passes).

- [ ] **Step 3: Implement** `ProcessRunner_win32.cpp` as described. Document at the declarations: the FILE_ID
  re-check and its residual; why the child's ends are inheritable only around the call; why `UnregisterWaitEx`
  blocks.

- [ ] **Step 4: Run to verify they pass**

Run: `cmake --build --preset clangcl-release --target vtpty_test && out/build/clangcl-release/bin/vtpty_test.exe "[processrunner]"`
Expected: `All tests passed`, 27 test cases in `[processrunner]` (26 here + the probe smoke case). Then the
whole suite: `out/build/clangcl-release/bin/vtpty_test.exe` → `All tests passed`, with Task 2's case count + 25
(26 added, the "unsupported" case deleted).

- [ ] **Step 5: Commit**

```bash
git add src/vtpty/ProcessRunner_win32.cpp src/vtpty/ProcessRunner_win32_test.cpp src/vtpty/ProcessRunner_test.cpp src/vtpty/CMakeLists.txt
git commit -m "feat(vtpty): run processes on Windows

PipedProcessRunner starts an .exe found on the absolute entries of the
spec's own PATH, suspended, in a kill-on-close job, with only NUL and two
pipe ends inherited, in the held directory (re-checked by file id around
CreateProcessW), with the spec's environment alone; both streams are read
through one completion port and kept up to the cap.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

### Task 4: Ending processes — timeout, cancel, orphans, destruction, the daemon's death

**Files:**
- Modify: `src/vtpty/ProcessRunner_win32.cpp`, `src/vtpty/ProcessRunner_win32_test.cpp`
- Modify: `src/vtpty/ProcessRunner.hpp` (Doxygen only: the Windows behaviour of `ProcessCompletion`'s
  enumerators, `cancel()`, `ProcessRunnerTuning::terminateGrace`, `PipedProcessRunner`'s class comment,
  `ProcessRunnerError::WakeChannelUnavailable` — "the pipe, or on Windows the completion port")
- Modify: `docs/extensions.md` (a Windows note under the orchestrator's requirements: programs must be `.exe`;
  `npm`'s `claude.cmd` is refused, use the native installer; a timed-out `git` may leave `index.lock`), and the
  roadmap's table row B ("done") and its "Pre-existing Windows test failures" if any changed

**Behaviour:**
- `terminate(child, reason)`: `endedBy = reason`, `ending = Killed`, `TerminateJobObject(job, 1)`. Immediate
  (Decision 2). `cancel()` calls it for a live, not-yet-ended token and wakes the pump; anything else ignored.
- Deadlines in `applyDeadlines(now)`: `NotAsked && now >= deadline` → terminate `TimedOut`;
  `Leader::Exited && a stream open && now >= leaderExitedAt + orphanedOutputGrace` → `TerminateJobObject`, then
  close the streams keeping what was read (`CancelIoEx` + `GetOverlappedResult(…, TRUE)` on a pending read
  first). `nextDeadline()` as POSIX, without the `Terminating` branch.
- Completion: the exit (Decision 3) — `exitCode` kept only for a leader that exited before `terminate`.
- `~PipedProcessRunner()`: for every child, `TerminateJobObject`; `UnregisterWaitEx(…, INVALID_HANDLE_VALUE)`;
  cancel and await pending reads; wait for the leader at most 1 s (`KillGracePeriod`, as `CommandCapture`);
  close the job. No handler is called. Then the port.

**Tests** (appended to `ProcessRunner_win32_test.cpp`; a test helper `isDead(DWORD pid)` opens
`SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION` and waits up to 5 s — a failed open counts as dead — and an
`awaitPid(file)` as POSIX's):

```cpp
TEST_CASE("PipedProcessRunner: a timeout terminates", "[processrunner]")
{   // {"probe","pid",pidFile,"sleep","600000"}; awaitPid; running() == 1; clock.advance(30s + 1ms);
    // runToCompletion → TimedOut, no exitCode, no signal; clock.now() < 30s + 1ms + terminateGrace
    // (no grace waited); isDead(pid)
}
TEST_CASE("PipedProcessRunner: a timeout kills a grandchild that holds no pipe", "[processrunner]")
{   // {"probe","spawn-quiet",grandFile,"pid",readyFile,"sleep","600000","sleep","600000"}; awaitPid(grandFile)
    // and readyFile; advance past the timeout; → TimedOut; isDead(grandchild)
}
TEST_CASE("PipedProcessRunner: cancel", "[processrunner]")
{   // as POSIX: Cancelled, no signal; cancelling the completed token again is ignored; running() == 0
}
TEST_CASE("PipedProcessRunner: cancelling a process that already exited keeps its exit code", "[processrunner]")
{   // tuning.orphanedOutputGrace = 1h; {"probe","spawn",grandFile,"sleep","600000","exit","6"}: the leader
    // exits 6, the grandchild holds the pipe; pump until the leader's exit is seen (pump a few rounds after
    // awaitPid); cancel → Cancelled with exitCode 6; isDead(grandchild)
}
TEST_CASE("PipedProcessRunner: an orphaned grandchild does not hold the reply", "[processrunner]")
{   // {"probe","spawn",grandFile,"sleep","600000","out","done"}, timeout 1h; each round advances the clock by
    // orphanedOutputGrace → Exited 0, stdout "done\n"; isDead(grandchild)
}
TEST_CASE("PipedProcessRunner: destruction kills the process tree", "[processrunner]")
{   // {"probe","spawn",grandFile,"sleep","600000","pid",pidFile,"sleep","600000"}; await both;
    // runner.reset(); isDead(leader) && isDead(grandchild); finished empty
}
TEST_CASE("PipedProcessRunner: the children die with the process that runs them", "[processrunner]")
{   // CreateProcessW (no job, test-side) VTPTY_PROCESS_RUNNER_HOST with <bin\probe.exe> <pidFile>; wait for it
    // (30 s) → exit code 7; read pidFile; isDead(probe pid)
}
TEST_CASE("PipedProcessRunner: an inheritable handle of the daemon does not reach the child", "[processrunner]")
{   // CreatePipe(&read, &write, &inheritable, 0) in the test; start {"probe","sleep","600000"}; pump once;
    // CloseHandle(write); PeekNamedPipe(read, …) fails with ERROR_BROKEN_PIPE (no one else holds `write`);
    // cancel; runToCompletion → Cancelled
}
```

Not ported, with the reason in a comment at the top of the file: "a child ignoring SIGTERM is killed after the
grace" (no graceful phase, Decision 2), both "leader reaped by someone else" cases (impossible: the runner
holds the handle), "a child has no controlling terminal" (POSIX sessions), "a relative program is found in the
directory entered by handle" (Decision 5; its refusal is tested in Task 3).

- [ ] **Step 1: Write the tests above.**

- [ ] **Step 2: Run to verify they fail**

Run: `cmake --build --preset clangcl-release --target vtpty_test && out/build/clangcl-release/bin/vtpty_test.exe "[processrunner]"`
Expected: the timeout, cancel and orphan cases fail by "the process did not complete in time"; the destruction
case may pass already (closing the job kills the tree) — note which, and keep it as the regression it is.

- [ ] **Step 3: Implement** the behaviour above, the header Doxygen, the docs.

- [ ] **Step 4: Run to verify they pass**

Run: `cmake --build --preset clangcl-release --target vtpty_test && out/build/clangcl-release/bin/vtpty_test.exe "[processrunner]"`
Expected: `All tests passed`, 35 test cases. Repeat 20 times to shake out ordering:
`for i in $(seq 20); do out/build/clangcl-release/bin/vtpty_test.exe "[processrunner]" | tail -1; done` — 20 ×
`All tests passed`. Then the whole suite: `out/build/clangcl-release/bin/vtpty_test.exe` → `All tests passed`.

- [ ] **Step 5: Downstream build check** (the runner's consumers compile against an unchanged header, but link
  the new code): `cmake --build --preset clangcl-release --target contour vthost_test` → builds without warnings.
  `vthost_test.exe "[daemon]"`: no case fails that did not fail at the roadmap's recorded baseline.
  (`vtextension_test` does not compile on Windows — pre-existing, roadmap — so `DeclaredProcesses`' real-process
  test stays POSIX-only; porting it is phase E's end-to-end run.)

- [ ] **Step 6: Commit**

```bash
git add src/vtpty/ProcessRunner_win32.cpp src/vtpty/ProcessRunner_win32_test.cpp src/vtpty/ProcessRunner.hpp docs/extensions.md docs/superpowers/plans/2026-11-23-orchestrator-windows-glab-acli-roadmap.md
git commit -m "feat(vtpty): end Windows processes and their trees on timeout and cancel

A timed-out or cancelled process's job is terminated at once (Windows has
no SIGTERM to grant a grace for); a leader that exited while a grandchild
holds its pipe completes after the orphan grace; destruction, or the
daemon dying, closes each job and kills what is left.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

## After the last task

- Run `/simplify` over the branch diff (AGENT.md workflow) and rebuild `vtpty_test`.
- Raise the three gaps in "Out of scope" (StdioChild's handle list, `childEnvironment()`'s case, the root
  grammar) in the roadmap.
- Report in the change summary: performance — none on the terminal path; per command one job, two pipes and a
  thread-pool wait, and an idle runner thread blocks in `GetQueuedCompletionStatusEx` without polling. Risk —
  Windows-only code behind an unchanged header, plus the ConPty pipe extraction (covered by `ConPty_test`).
  Coverage — the Windows runner by `ProcessRunner_win32_test.cpp`; the spawn rules on every platform.
