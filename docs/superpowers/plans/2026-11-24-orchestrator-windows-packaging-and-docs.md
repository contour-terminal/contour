# E — Orchestrator on Windows: Packaging and Docs Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The Windows MSI, per-user MSI and zip (and so winget) carry `share/contour/extensions/orchestrator.wasm`;
`docs/extensions.md`, the daemon WIT and the orchestrator README describe Windows instead of excluding it; the
code follow-ups earlier reviews assigned to phase E land; and a scripted smoke check shows the daemon, the
orchestrator, a GitLab repository and a JQL source working together on a Windows machine.

**Architecture:** Nothing in the install rules stops the guest shipping on Windows: `src/contour/CMakeLists.txt`
already calls `contour_install_bundled_extensions("share/contour/extensions")` in its `WIN32` branch, the daemon's
Windows staging plan (`src/vthost/DaemonImage.cpp`) copies `share/contour` beside `bin`, and
`vtextension::bundledExtensionDirectories(Windows)` looks in `<exe>/../share/contour/extensions`. What stops it is
CI: neither Windows job configures `CONTOUR_EXTENSIONS_ORCHESTRATOR_GUEST=ON` nor fetches a wasi-sdk, and
`scripts/check-windows-package.ps1` does not require the module. Phase E turns the guest on in both Windows jobs
(the wasi-sdk composite action learns Windows), makes the package check require the module, and changes nothing
in CMake. Docs are written against `HEAD` and reach the index without passing through the working tree, so the
user's uncommitted hunks in `docs/extensions.md` are never staged, reverted or rewritten.

**Tech Stack:** GitHub Actions (composite action, bash + pwsh), CPack WIX/ZIP (unchanged), C++23 + Catch2
(`contour_test`, `orchestrator_test`, `vthost_test`, `vtpty_test`), wit-bindgen 0.63.0, PowerShell 7 (smoke
script), git plumbing (`hash-object`, `update-index --cacheinfo`, `merge-file`).

**Spec:** `docs/superpowers/plans/2026-11-23-orchestrator-windows-glab-acli-roadmap.md`, phase 6, "E — Packaging
and docs": ship the guest in Windows packages; drop the Windows limitation from `docs/extensions.md`; an
end-to-end run on this machine — daemon, orchestrator, a GitLab repo, a JQL source. Doc text was collected by
phases B–D under "Docs for phase E" (in their plans and in the session's collection, reproduced in Task 10).

## Depends on

Phases 1, A, A2, B, C and D, all landed on `fix/daemon-echo-hidden-cursor` (`git log --oneline 8c59528df..HEAD`).

## Global Constraints

- **The user's uncommitted work.** `docs/extensions.md` and `cmake/ContourWasiCxx.cmake` carry the user's own
  uncommitted hunks (`git diff -- docs/extensions.md cmake/ContourWasiCxx.cmake`: the CPM-fetched wasi-sdk and
  its documentation in "Building a C++ guest"). For the whole plan:
  - never `git add -A`, `git add .`, `git add -u`, `git commit -a`, or `git commit -- <path>` / `git commit <path>`
    (a pathspec commit takes the **working-tree** content); stage named paths only, and run
    `git diff --cached --name-only` before every commit — it must list exactly the task's files;
  - never `git stash`, `git checkout -- <file>`, `git restore`, `git reset --hard`, or a formatter/`sed -i` on
    either file;
  - `cmake/ContourWasiCxx.cmake` is not edited by this plan at all;
  - `docs/extensions.md` is changed only by Task 10's procedure.
- **Pre-existing state.** `out/build/clangcl-release` is configured with the guest on (`WASI_SDK_PREFIX` empty: its
  wasi-sdk came from the user's uncommitted CPM fetch, `_deps/wasi-sdk-src`). Use that tree; do not reconfigure it,
  and do not delete `_deps`.
- Project rules: `AGENT.md` (C++23, range-for only, `[[nodiscard]]`, Doxygen on new public API, `enum class` over
  `bool`, no `NOLINT`, zero warnings under `-Werror`, clang-format). Never edit `vendor/core-cpp`.
- Builds take minutes: run them with a 10-minute timeout, one at a time (another agent may share the machine):
  `cmake --build --preset clangcl-release --target <target>`; binaries are `out/build/clangcl-release/bin/<t>.exe`.
- **No real user data in committed files.** No user name, home path, GitLab host, project, Jira site, JQL or
  issue key in the repository. The smoke script takes them as parameters and writes them only under its temporary
  root. Examples in docs use `C:\Users\me\…`, `https://example.atlassian.net`, `gitlab.example.com`.
- Commit messages end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Subagent models (user's instructions): Haiku for Tasks 2, 3, 4, 6, 8, 9 (fully specified); Opus for Tasks 1, 5,
  7, 10, 11, 12 (measurement, judgement, the WIP-preserving procedure); every review on Opus.

## Decisions

1. **CI turns the guest on with command-line flags in the two Windows jobs, not in the `msvc-release` preset.** The
   macOS precedent sets it in `macos-package`; on Windows `msvc-release` is also what developers configure
   locally, and with the guest in the preset a local configure without `WASI_SDK_PREFIX` fails until the user's
   uncommitted CPM fetch lands. Passing `-DCONTOUR_EXTENSIONS_ORCHESTRATOR_GUEST=ON -DWASI_SDK_PREFIX=…` from the
   jobs works with or without that work. Moving it into the preset is a one-line follow-up once the fetch is
   committed.
2. **Both Windows jobs build the guest** — `windows` (pull requests, `msvc-ci`) and `windows_package`. Only the
   second ships, but it does not run on pull requests; building the guest in the first means `DaemonWorldE2E`,
   the guest-driven `vtextension_test` cases and the packaged module are checked before a merge, as the Linux jobs
   do. Cost: a 200–600 MB download and the guest's sub-build per run. **Gate:** Task 1 must show the guest-on
   Windows suite green (or each failure fixed or explicitly listed) — the windows-tests action runs all of
   `ctest`, so a guest-on failure would turn both jobs red.
3. **The wasi-sdk action extracts on Windows with `cmake -E tar`,** the extractor that unpacked the same tarball
   locally (CPM), and reports the prefix with forward slashes (`cygpath -m`); Git Bash's GNU `tar` reads `D:` as
   a remote host. Linux and macOS keep their current commands. The Windows digest is pinned as
   `WASI_SDK_X86_64_WINDOWS_SHA256` beside the other two, verified against the release (Task 7 Step 1).
4. **A non-`.exe` `programs.<name>` on Windows is kept, with a warning at config load** — not dropped. Dropping it
   would fall back to a `PATH` lookup that might run a different `claude.exe` than the one configured; keeping it
   preserves today's behaviour (the host refuses it at start) and the warning says why and what to do. The rule is
   `vtpty::windowsImageName()`, the very rule the runner applies, so the warning cannot disagree with the refusal.
5. **Negative exit codes are shown in hex on every platform** (`it exited with 0xC0000005`). A negative 32-bit
   code is an NTSTATUS on Windows and never produced on POSIX (signals are reported separately), so the rule needs
   no platform switch and is tested everywhere.
6. **Docs: one new subsection, "Daemon extensions on Windows",** placed right before "Daemon extensions: granting",
   holds the host-side Windows rules (roots, environment, `path:` spelling, programs, timeouts, `pane-open`,
   `file-read`); the orchestrator section gets its own short "On Windows" paragraph (YAML quoting, `claude.exe`,
   `index.lock`, worktree lifetime). The remaining edits are deletions of "Windows" from the not-supported lists.
   This keeps every changed line far (> 150 lines) from the user's hunk at lines 262–270.
7. **The WIT doc comments change, and the bindings are regenerated** with wit-bindgen 0.63.0 (`cargo install
   wit-bindgen-cli --version 0.63.0 --locked`). The regenerated diff must be comment-only; anything else is a
   stop-and-report (a generator drift, not this phase's change).
8. **The phase-D re-review follow-ups are in scope** (the session's collection lists them for phase E): the
   non-held `Process::start()` pseudoconsole leak (Task 5), `releaseUnadopted()` closing crossed handle values
   unchecked, and the 1 s margin of `ConPty.aDeferredJobCloseGivesALeaderThatStaysTheGrace` (Task 6).
9. **The smoke check is a committed PowerShell script** (`scripts/smoke-orchestrator-windows.ps1`) with three
   tiers: `Offline` (no account: packaging, config, grant, roots, dispatch, panes, board, job lifetime, live
   upgrade), `Claude` (adds a logged-in Claude Code: transcript and the agent's own `STATUS.md`), `Forge` (adds the
   user's GitLab clone and a JQL source). It isolates the daemon's state by running daemon and window with
   `LOCALAPPDATA` pointing into its temporary root, after checking that `glab`/`acli` authentication does not
   change under that override. The grant itself stays manual: by design no command can grant.

## Review Focus

- Task 10: the index-only commit and the three-way merge into the working tree — re-run the verification block
  yourself; `git diff -- docs/extensions.md` must show exactly the user's original lines.
- Task 7: both Windows jobs get the wasi-sdk step **and** the flags on every `cmake --preset` call they make (the
  `windows` job may configure twice for the compile cache); the action stays byte-identical in behaviour on Linux
  and macOS.
- Task 2: the warning fires only on Windows, only for a refused extension, and keeps the entry.
- Task 5/6: a pseudoconsole started and then abandoned is always closed; a crossed handle value is closed only
  after it is shown to be what it claims.
- Every committed file: no personal path, host, site or JQL.

---

### Task 1: Measure the guest-on Windows suite (the gate for Task 7)

**Files:** none committed (results feed Task 12's roadmap update).

- [ ] **Step 0: Fingerprint the user's work** (re-checked by Task 10 and at the end):
  `git diff -- docs/extensions.md | sha256sum > <scratchpad>/user-docs.sha` and
  `git diff -- cmake/ContourWasiCxx.cmake | sha256sum > <scratchpad>/user-cmake.sha`.
- [ ] **Step 1:** Build what `ctest` runs: `cmake --build --preset clangcl-release` (10-minute timeout; repeat if it
  times out mid-build — it resumes).
- [ ] **Step 2:** `ctest --test-dir out/build/clangcl-release -j 8 --output-on-failure 2>&1 | tee
  <scratchpad>/phaseE-ctest.txt`. Record every failing test and, for Catch2 suites, the failing cases
  (`out/build/clangcl-release/bin/<suite>.exe --reporter compact 2>&1 | findstr /C:"failed"`).
- [ ] **Step 3:** Compare with the roadmap's "Pre-existing Windows test failures". Expected changes to confirm or
  refute: `vtextension_test` now compiles (`ee96175f4`); `DaemonConsentKey_test.cpp:140` was fixed by A2;
  `DaemonWorldE2E_test.cpp:196` (`status("orchestrator")` empty) — A2 made `C:\` roots resolve, so it may pass now;
  `StdioChild_test.cpp:145` is environmental (Git Bash's `sort.exe`) and only fails when run from Git Bash — run it
  once from pwsh to tell.
- [ ] **Step 4: Gate.** If any guest-only case fails (`DaemonWorldE2E`, `DaemonGuestModule_test`, the
  `cxx-daemon-probe` cases, `orchestrator_test`), stop and report it with its output before Task 7: either it is
  fixed first (a separate task, Opus), or the human decides to ship with only `windows_package` building the
  guest. No commit.

### Task 2: Warn when a program is not an `.exe` on Windows

**Files:**
- Modify: `src/contour/config/Config.cpp` (`YAMLConfigReader::readDaemonExtensionPrograms`, ~:3983)
- Test: `src/contour/config/DaemonSettings_test.cpp`

- [ ] **Step 1: Write the failing test** (Windows only; on POSIX `claude.cmd` is an ordinary file name):

```cpp
#ifdef _WIN32
TEST_CASE("daemon.extensions: a program that is no .exe is kept, and warned about, on Windows", "[config][daemon]")
{
    auto capture = core::log::ScopedCapture { "error" };
    auto const config = loadConfigFromYaml("daemon:\n"
                                           "  extensions:\n"
                                           "    orchestrator:\n"
                                           "      programs:\n"
                                           "        claude: 'C:\\Users\\me\\AppData\\Roaming\\npm\\claude.cmd'\n"
                                           "        gh: 'C:\\Program Files\\GitHub CLI\\gh.exe'\n"
                                           "        git: 'C:/Program Files/Git/cmd/git'\n");
    REQUIRE(config.daemon.value().extensions.size() == 1);
    CHECK(config.daemon.value().extensions.front().programs.size() == 3); // kept: the host refuses it at start
    auto const& log = capture.text();
    CHECK(log.contains("Program 'claude' of extension 'orchestrator'"));
    CHECK(log.contains("only an .exe"));
    CHECK_FALSE(log.contains("'gh'"));  // an .exe
    CHECK_FALSE(log.contains("'git'")); // no extension: .exe is appended
}

TEST_CASE("daemon.extensions: a program may start with ~\\ on Windows", "[config][daemon]")
{
    auto const config =
        loadConfigFromYaml("daemon:\n  extensions:\n    orchestrator:\n      programs:\n        gh: '~\\bin\\gh.exe'\n");
    auto const& programs = config.daemon.value().extensions.front().programs;
    REQUIRE(programs.size() == 1);
    CHECK(programs.front().path.is_absolute());
    CHECK(programs.front().path.filename() == "gh.exe");
}
#endif
```

  Add `#include <core/log/LogSink.hpp>` if the file lacks it.
- [ ] **Step 2:** `cmake --build --preset clangcl-release --target contour_test`, then
  `out/build/clangcl-release/bin/contour_test.exe "daemon.extensions: a program*"` → the first new case fails (no
  warning); the `~\` case passes already (`core::homeResolvedPath` accepts `~\`) and pins it.
- [ ] **Step 3: Implement.** In the loop, after the absolute-path check and before `push_back`:

```cpp
#ifdef _WIN32
        // The host runs only an .exe for an extension (vtpty::windowsImageName, the rule it refuses by): a
        // script such as npm's claude.cmd would be refused only when the extension first runs it.
        if (!vtpty::windowsImageName(core::pathToUtf8(path.filename())))
            errorLog()("Program '{}' of extension '{}' of {}: '{}' is not an .exe, and Windows runs only an .exe "
                       "for an extension, so it will be refused when run. Point it at the program's .exe (Claude "
                       "Code's native installer provides claude.exe).",
                       program,
                       name,
                       keyPath,
                       core::pathToUtf8(path));
#endif
```

  Include `<vtpty/WindowsProcessRules.hpp>` (under `#ifdef _WIN32`); `contour_config` links `vtbackend`, which
  links `vtpty`. Also widen the absolute-path error text to "…must be an absolute path or start with '~/' (or
  '~\\' on Windows)." — check `grep -rn "or start with '~/'" src` for a test asserting the old text and update it.
- [ ] **Step 4:** Rebuild `contour_test`; run `contour_test.exe "[daemon]"` → all pass. clang-format the two files.
- [ ] **Step 5: Commit**

```bash
git add src/contour/config/Config.cpp src/contour/config/DaemonSettings_test.cpp
git diff --cached --name-only   # exactly these two
git commit -m "feat(config): warn at load when an extension's program is no .exe on Windows

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

### Task 3: The orchestrator shows a negative exit code in hex

**Files:**
- Modify: `extensions/orchestrator/core/Engine.cpp` (`judge`, ~:54)
- Test: `extensions/orchestrator/core/Engine_test.cpp` (already includes `EngineDetail.hpp`)

- [ ] **Step 1: Failing test**, beside the other `[orchestrator][engine]` cases (uses the file's `failed()` helper):

```cpp
TEST_CASE("a command's exit code is shown in decimal, an NTSTATUS in hex", "[orchestrator][engine]")
{
    CHECK(engine::detail::judge(failed(1)).why == "it exited with 1");
    CHECK(engine::detail::judge(failed(128)).why == "it exited with 128");
    CHECK(engine::detail::judge(failed(-1073741819)).why == "it exited with 0xC0000005"); // access violation
    CHECK(engine::detail::judge(failed(-1)).why == "it exited with 0xFFFFFFFF");
    CHECK(engine::detail::judge(failed(-1073741819, "fatal: boom\nmore")).why == "fatal: boom"); // stderr wins
}
```

- [ ] **Step 2:** `cmake --build --preset clangcl-release --target orchestrator_test`; run
  `orchestrator_test.exe "a command's exit code*"` → the hex checks fail.
- [ ] **Step 3: Implement** a named helper in `engine::detail`'s anonymous scope of `Engine.cpp`:

```cpp
    /// @return How an exit code reads: decimal, or for a negative one (an NTSTATUS on Windows, such as
    ///         0xC0000005) its 32 bits in hex.
    [[nodiscard]] std::string exitCodeText(int32_t code)
    {
        return code < 0 ? std::format("0x{:08X}", static_cast<uint32_t>(code)) : std::format("{}", code);
    }
```

  and use `std::format("it exited with {}", exitCodeText(*outcome.exitCode))`.
- [ ] **Step 4:** Run `orchestrator_test.exe` → all pass. clang-format.
- [ ] **Step 5: Commit** `extensions/orchestrator/core/Engine.cpp extensions/orchestrator/core/Engine_test.cpp`:
  `fix(orchestrator): show a negative exit code, an NTSTATUS, in hex`.

### Task 4: A Windows profile-route test for pane-open

**Files:** Test: `src/vthost/ShellPtyFactory_test.cpp` (the `#elifdef VTHOST_PROCESS_RUNNER_PROBE` block)

- [ ] **Step 1:** Move the four line checks of "a host without profiles spawns an agent in the handle's directory,
  on Windows" into a helper inside the `#ifdef VTHOST_PROCESS_RUNNER_PROBE` helper block:

```cpp
/// Checks the probe's report of an agent started in @p worktree: its directory first, GIT_DIR and GH_REPO
/// withheld, GIT_SSH_COMMAND kept.
void checkAgentReport(HeldWorktree const& worktree, std::string_view report)
{
    auto const lines = linesOf(report);
    REQUIRE_FALSE(lines.empty());
    CHECK(lines.front() == worktree.handle->resolvedPath());
    CHECK(std::ranges::none_of(lines, [](std::string_view line) { return line.starts_with("GIT_DIR="); }));
    CHECK(std::ranges::none_of(lines, [](std::string_view line) { return line.starts_with("GH_REPO="); }));
    CHECK(std::ranges::contains(lines, std::string { "GIT_SSH_COMMAND=ssh -i kept" }));
}
```

  and call it from that case. Add the profile route, mirroring the POSIX "a profile's spawn keeps them too":

```cpp
TEST_CASE("a profile's spawn starts an agent in the handle's directory, on Windows", "[vthost][spawn][profiles]")
{
    auto const worktree = HeldWorktree {};
    auto const inherited = InheritedRepositorySelection {};
    auto shell = vthost::testing::fakeProfile(DefaultShell, vtbackend::LineCount(100));
    shell.sandbox = vthost::SandboxEscape::Contained;
    auto h = SpawningHost { std::make_shared<vthost::testing::FakeProfileCatalog>(
        std::map<std::string, vthost::ResolvedProfile> { { "main", std::move(shell) } }, "main") };

    auto const* tab = h.host.createTab(
        { .commandOverride = agentOverride(worktree), .startDirectory = vthost::StartDirectory::Required });
    REQUIRE(tab != nullptr);

    checkAgentReport(worktree, awaitFile(worktree.report(), [&] { h.turn(); }));
}
```

- [ ] **Step 2:** `cmake --build --preset clangcl-release --target vthost_test`; run
  `vthost_test.exe "[spawn]"` → all pass (the new case exercises `makeProfilePtyFactory` with a held directory; if
  it fails, stop: that is a product defect in the profile route, report it with the probe's report).
- [ ] **Step 3: Commit** the test file: `test(vthost): an agent spawned through a profile on Windows`.

### Task 5: A non-held Windows start that fails after its pseudoconsole started closes it

**Files:** Modify: `src/vtpty/Process_win32.cpp` (`Process::start`, ~:440–545); Test: `src/vtpty/ConPty_test.cpp`

The held path got a `gsl::finally` guard in `d4b2725f0`; the non-held path still returns `SpawnFailed` with the
pseudoconsole started after `_d->pty->start()` — on the environment-not-UTF-8 return and when every ladder rung
fails — and ignores `initializeStartupInfoAttachedToPTY`'s `HRESULT`.

- [ ] **Step 1: Failing test**, next to `ConPty.aProgramNotCreatedInAHeldDirectoryClosesItsPseudoconsole`:

```cpp
// A spawn refused after its pseudoconsole started -- here for an environment that is not UTF-8 -- closes it:
// a started one with no program on it would read nothing forever, and its pane would stay open and empty.
TEST_CASE("ConPty.aStartRefusedAfterItsPseudoconsoleStartedClosesIt", "[vtpty][conpty]")
{
    auto exec = vtpty::testing::execInfo("cmd.exe", { "/c", "exit" });
    exec.env = { { "CONTOUR_NOT_UTF8", "\xFF" } };
    auto process = vtpty::testing::unplacedProcess(exec, pageSize(80, 24));
    auto const started = process.start();
    REQUIRE_FALSE(started.has_value());
    CHECK(started.error().error == vtpty::StartError::SpawnFailed);
    CHECK(process.pty().isClosed());
}
```

  (Use the helpers the neighbouring held cases use; `execInfo` is `vtpty::testing`'s.)
- [ ] **Step 2:** `cmake --build --preset clangcl-release --target vtpty_test`; `vtpty_test.exe
  "ConPty.aStartRefused*"` → fails on `isClosed()`.
- [ ] **Step 3: Implement** in `Process::start()`, right after `_d->pty->start()` succeeds, the same guard as the
  held path (`auto running = false; auto const closeUnlessRunning = gsl::finally(...)`), set `running = true` just
  before the successful `return StartOutcome{…}`; and check the attach:
  `if (auto const attached = initializeStartupInfoAttachedToPTY(…); FAILED(attached)) return std::unexpected(
  StartFailure { .error = StartError::SpawnFailed, .detail = std::format("Could not attach it to its
  pseudoconsole: {}", hresultMessage(attached)) });` — reuse the held path's `hresultMessage`; if it is local to
  the held branch, hoist it to the file's anonymous namespace rather than copy it.
- [ ] **Step 4:** `vtpty_test.exe "[conpty]"` and the whole `vtpty_test.exe` → all pass. clang-format.
- [ ] **Step 5: Commit** `src/vtpty/Process_win32.cpp src/vtpty/ConPty_test.cpp`:
  `fix(vtpty): a Windows start refused after its pseudoconsole started closes it`.

### Task 6: Check a crossed handle before closing it; widen a timing margin

**Files:** Modify: `src/vtpty/PtyAdoption.cpp` (`releaseUnadopted`); Test: `src/vtpty/PtyAdoption_test.cpp`,
`src/vtpty/ConPty_test.cpp` (`ConPty.aDeferredJobCloseGivesALeaderThatStaysTheGrace`)

`releaseUnadopted()` closes `child.processHandle` and `child.job` as raw values that crossed an exec; in the
`!canAdopt` and `abandon` paths of `src/vthost/handover/HandoverRestore.cpp` those values may name something else
in this process by now.

- [ ] **Step 1: Failing tests** (`#ifdef _WIN32` in `PtyAdoption_test.cpp`):

```cpp
TEST_CASE("releaseUnadopted leaves a value that names no job open", "[vtpty][adoption]")
{
    auto* const event = CreateEventW(nullptr, TRUE, FALSE, nullptr); // what the job's value names now
    REQUIRE(event != nullptr);
    vtpty::releaseUnadopted(vtpty::AdoptedChild { .job = vtpty::CrossingHandle { event } });
    auto flags = DWORD {};
    CHECK(GetHandleInformation(event, &flags)); // still open
    CloseHandle(event);
}

TEST_CASE("releaseUnadopted leaves a process handle of another process open", "[vtpty][adoption]")
{
    auto self = HANDLE {};
    REQUIRE(DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &self, 0, FALSE,
                            DUPLICATE_SAME_ACCESS));
    vtpty::releaseUnadopted(vtpty::AdoptedChild { .pid = 4, .processHandle = vtpty::CrossingHandle { self } });
    auto flags = DWORD {};
    CHECK(GetHandleInformation(self, &flags));
    CloseHandle(self);
}
```

  Construct `CrossingHandle` the way the file's existing cases do (adjust the spelling, not the intent), and add a
  positive case: a real job (`CreateJobObjectW`) and the current process's duplicated handle with
  `.pid = GetCurrentProcessId()` are both closed (`GetHandleInformation` fails afterwards).
- [ ] **Step 2:** Build `vtpty_test`; run `vtpty_test.exe "[adoption]"` → the two negative cases fail.
- [ ] **Step 3: Implement** two predicates in `PtyAdoption.cpp`'s anonymous namespace, `#ifdef _WIN32`:
  `namesJob(HANDLE)` — `QueryInformationJobObject(h, JobObjectBasicAccountingInformation, …)` succeeds;
  `namesProcess(HANDLE, std::int64_t pid)` — `GetProcessId(h) == pid` and `pid > 0`. Close each value only when
  its predicate holds; update the header comment ("closed once shown to be the job and the process they crossed
  as; a value that names something else by now is left alone").
- [ ] **Step 4:** In `ConPty.aDeferredJobCloseGivesALeaderThatStaysTheGrace`, raise `Grace` to `5s` and sleep only
  to `deferred + 1s` before the two `isAlive` checks (4 s of margin instead of 1 s); keep the later checks.
- [ ] **Step 5:** `vtpty_test.exe` whole, and `vthost_test.exe "[handover]"` → no new failure. clang-format.
- [ ] **Step 6: Commit** the three files: `fix(vtpty): close a crossed handle only once it is what it crossed as`.

### Task 7: Ship the guest in the Windows packages

**Files:**
- Modify: `.github/actions/wasi-sdk/action.yml`
- Modify: `.github/workflows/build.yml` (`env`, jobs `windows` and `windows_package`)
- Modify: `scripts/check-windows-package.ps1`

Precondition: Task 1's gate passed.

- [ ] **Step 1: Verify the Windows digest.**
  `gh release view wasi-sdk-34 -R WebAssembly/wasi-sdk --json assets --jq '.assets[] | select(.name ==
  "wasi-sdk-34.0-x86_64-windows.tar.gz") | .digest'` → `sha256:cccb5c323a9b34f0349a9b09e8804a0a7632c68c3310f4b5f437ed57d7e71d8f`
  (the same value the user's uncommitted `ContourWasiCxx.cmake` table holds — read it, do not edit it). A
  mismatch: stop and report.
- [ ] **Step 2: The action learns Windows.** In `action.yml`'s script: compute `temp="$RUNNER_TEMP"`, and on
  `RUNNER_OS == Windows` `temp=$(cygpath -u "$RUNNER_TEMP")`; download to `$temp`; check with `sha256sum` when it
  exists, else `shasum -a 256` (macOS); extract with `tar -xzf … -C "$temp"` except on Windows, where
  `(cd "$temp" && cmake -E tar xzf "$name.tar.gz")` (Decision 3); test `bin/clang++` or `bin/clang++.exe`; output
  `prefix=$(cygpath -m "$temp/$name")` on Windows, `$RUNNER_TEMP/$name` elsewhere. Update the description's
  platform list (`x86_64-windows`). Linux/macOS commands must stay as they are apart from the `sha256sum`
  preference.
- [ ] **Step 3: The workflow.** Add `WASI_SDK_X86_64_WINDOWS_SHA256: "cccb5c32…d8f"` under `WASI_SDK_ARM64_MACOS_SHA256`.
  In both `windows` and `windows_package`, add before "Generate build files":

```yaml
      # The agent orchestrator's guest is C++, built with wasi-sdk (cmake/ContourWasiCxx.cmake), and installed with
      # the bundled extensions into share/contour/extensions. On the command line rather than in the preset, so
      # that a local msvc-release configure needs no wasi-sdk.
      - name: "wasi-sdk (the agent orchestrator)"
        id: wasi_sdk
        uses: ./.github/actions/wasi-sdk
        with:
          version: ${{ env.WASI_SDK_VERSION }}
          platform: x86_64-windows
          sha256: ${{ env.WASI_SDK_X86_64_WINDOWS_SHA256 }}
```

  and append `-DCONTOUR_EXTENSIONS_ORCHESTRATOR_GUEST=ON "-DWASI_SDK_PREFIX=${{ steps.wasi_sdk.outputs.prefix }}"`
  to **every** `cmake --preset` invocation in those two jobs (`grep -n "cmake --preset" .github/workflows/build.yml`
  between the `windows:` and the next top-level job, and in `windows_package`; the MSI-upgrade check's
  `$earlierBuild` configures too — include it, or its earlier build ships no module and the upgrade check
  compares unlike packages).
- [ ] **Step 4: The package check requires it.** In `check-windows-package.ps1`, add `'orchestrator'` to the
  extension list of `$RequiredZipEntries` (and extend the comment above it: "and the agent orchestrator's
  module"). If the script also checks the MSI's file table for the extensions, add it there the same way.
- [ ] **Step 5: Local evidence** (no CI here): `python scripts/check-workflows.py` (and `actionlint` if the script
  invokes it) → clean; `pwsh -NoProfile -Command "[scriptblock]::Create((Get-Content -Raw
  scripts/check-windows-package.ps1)) | Out-Null"` → no parse error;
  `cmake --install out/build/clangcl-release --prefix <scratchpad>/phaseE-install` then
  `Test-Path <scratchpad>/phaseE-install/share/contour/extensions/orchestrator.wasm` → `True`. Run the action's
  bash body in Git Bash with `RUNNER_OS=Windows RUNNER_TEMP=<scratchpad>\wasi GITHUB_OUTPUT=<scratchpad>\out.txt`
  and the three inputs as env → `out.txt` holds `prefix=C:/…/wasi-sdk-34.0-x86_64-windows` and
  `<prefix>/bin/clang++.exe` exists (a 200+ MB download; skip with a note if offline).
- [ ] **Step 6: Commit** the three files: `ci(windows): ship the agent orchestrator in the Windows packages`.
  The first CI run of `windows` on the pushed branch is the real proof; report its result.

### Task 8: The daemon WIT speaks of Windows paths

**Files:** Modify: `wit/daemon/world.wit`; regenerate `extensions/orchestrator/guest/bindings/daemon.{c,h}`

- [ ] **Step 1:** Edit the doc comments (only comments):
  - `record root`, `paths`: `/// The root's directories: absolute, normalised, symlinks resolved, in the host's
    native spelling (`/home/me/repos`; on Windows `C:\Users\me\repos` or `\\server\share\repos`, backslashes,
    an upper-case drive letter, on-disk case and long names).`
  - `proc-start` and `pane-open`: append `` `cwd`, and a `path:` argument, are spelled as the root they lie in is. ``
  - `file-read`: `/// Reads a regular file inside a root declared readable. `path` is relative and
    `/`-separated on every platform.`
  - `not-supported-yet` (in `host-error`): unchanged; `file-error`'s `not-supported-yet` is no longer answered on
    Windows — leave the enumerator (ABI), no comment change needed.
- [ ] **Step 2:** `cargo install wit-bindgen-cli --version 0.63.0 --locked` (if `wit-bindgen --version` is not
  0.63.0), then from the repository root: `wit-bindgen c wit/daemon --world daemon --no-object-file --out-dir
  extensions/orchestrator/guest/bindings`.
- [ ] **Step 3:** `git diff --stat -- extensions/orchestrator/guest/bindings` and `git diff -U0 --
  extensions/orchestrator/guest/bindings | grep -E '^[-+][^-+]' | grep -vE '^[-+]\s*//'` → **no output** (comment
  lines only). Anything else: `git checkout -- extensions/orchestrator/guest/bindings` (these files carry no user
  work) and stop and report. Then `cmake --build --preset clangcl-release --target orchestrator_wasm vtextension_test`
  and `vtextension_test.exe "[daemon-world]"` (the WIT parse test; use the tag `DaemonWorld_test.cpp` declares) →
  pass.
- [ ] **Step 4: Commit** `wit/daemon/world.wit extensions/orchestrator/guest/bindings/daemon.c
  extensions/orchestrator/guest/bindings/daemon.h`: `docs(wit): a root's paths in the host's spelling, Windows included`.

### Task 9: The orchestrator README on Windows

**Files:** Modify: `extensions/orchestrator/README.md`

- [ ] **Step 1:** Add a section `## On Windows` at the end of the configuration part (before "LEFTOVER" or the
  first section about the dashboard — read the file and place it where configuration is described):

```markdown
## On Windows

The host hands the orchestrator `worktree_root` and each `repos[].path` in their resolved backslash spelling
(`C:\…` or `\\server\share\…`), whichever way the configuration wrote them (`C:\…`, `C:/…`, `\\server\share\…`,
`~\…`). The orchestrator works out the host's style from that spelling, refuses a repository spelled in the other
style or that Win32 cannot address, and compares roots ignoring ASCII case. It builds worktree paths with `\`
(`C:\…\wt\<repo>\<run>`) and reads git's forward-slash output against them; the paths it passes to `file-read`
stay `/`-separated and relative to the worktree root. A command that fails with a negative exit code — an
NTSTATUS — is shown in hex (`it exited with 0xC0000005`).
```

- [ ] **Step 2: Commit** the file: `docs(orchestrator): how the orchestrator reads Windows paths`.

### Task 10: `docs/extensions.md` — drop the Windows limitation, keep the user's work

**Files:** Modify: `docs/extensions.md` (committed version only; the working tree gets the same change merged in)

The working tree is CRLF (`core.autocrlf=true`), the index LF; the user's hunk sits at lines 262–270. Our change is
written against `HEAD`, put into the index as a blob, and merged into the working tree three-way. `S` is the
scratchpad directory (absolute, Git Bash spelling).

- [ ] **Step 1: Snapshot.**

```bash
cd /d/repo/contour
git diff --cached --quiet -- docs/extensions.md || { echo "STOP: docs/extensions.md has staged changes"; exit 1; }
cp docs/extensions.md "$S/extensions.user-wip.md"                       # backup of the working tree, CRLF
git diff -- docs/extensions.md > "$S/extensions.user.patch"
grep -E '^[-+]' "$S/extensions.user.patch" | grep -vE '^(\+\+\+|---) ' > "$S/extensions.user.lines"
git show HEAD:docs/extensions.md > "$S/extensions.ours.md"               # LF, edited below
git cat-file --filters HEAD:docs/extensions.md > "$S/extensions.base.crlf"  # HEAD as checked out (CRLF)
```

- [ ] **Step 2: Edit `$S/extensions.ours.md`** (the Edit tool on the scratch file; never on `docs/extensions.md`):
  1. **Daemon world, `pane-open` bullet** (~:425): "`pane-open` answers `not-supported-yet` where Contour cannot
     start a program in a directory it holds open (a Flatpak whose shells run on the host, and Windows), and `gone`
     …" → drop ", and Windows".
  2. **`file-read`** (~:522): replace "- On Windows, `file-read` answers `not-supported-yet`." with "- On Windows
     the same walk is made beneath the root's directory handle; see [Daemon extensions on
     Windows](#daemon-extensions-on-windows)."
  3. (~:524) "Today that is `file-read` on Windows, and `pane-open` where Contour cannot start a program in a
     directory it holds open (a Flatpak whose shells run on the host, and Windows)." → "Today that is `pane-open`
     where Contour cannot start a program in a directory it holds open: a Flatpak whose shells run on the host."
  4. **New subsection** right before `### Daemon extensions: granting`:

```markdown
### Daemon extensions on Windows

A daemon extension runs on Windows as on Linux and macOS, with these differences.

- **Roots.** A root's directory is drive-absolute (`C:\Users\me\repos` or `C:/Users/me/repos`), a UNC path
  (`\\server\share\repos` or `//server/share/repos`), `~`, or `~\…`/`~/…`. It is normalised to backslashes, an
  upper-case drive letter and no trailing separator. A drive-relative path (`C:repos`), a path rooted without a
  drive (`\repos`), a UNC path without a share, `~user`, a device or verbatim path (`\\?\…`, `\\.\…`, `\??\…`), and
  a component that is empty, `.` or `..` (refused, not collapsed), holds a stream (`a:b`), is a reserved device name
  (`CON`, `NUL`, …) or ends in a dot or a space are refused. The directory is resolved once -- junctions, symbolic
  links, mapped and `subst` drives and 8.3 short names followed -- and that spelling (on-disk case, long names) is
  what the consent dialog shows, what the grant is recorded for, and what the extension is told; re-spelling the
  same directory in the configuration does not ask again. Paths compare ignoring the case of ASCII letters.
- **Environment.** Variable names compare ignoring case, so `Git_Dir`, `contour_pane` or `gh_repo` are withheld
  exactly as `GIT_DIR`, `CONTOUR_PANE` and `GH_REPO` are. An inherited variable whose name holds a non-ASCII
  character is not passed to a declared command. Where `command-env`/`command-env-for` set a name that differs from
  an inherited one only in case, the declared spelling and value replace it.
- **`path:` arguments and working directories** are spelled with backslashes only, as the host's roots are; a
  forward slash is refused.
- **Programs.** A command or an agent pane runs only a program whose file name ends in `.exe`, looked up on the
  absolute entries of its `PATH` (relative entries, the current directory and `PATHEXT` are ignored), so that no
  argument ever passes through `cmd.exe`. A batch file such as npm's `claude.cmd` is refused as not installed, and
  a `programs` entry naming one is warned about when the configuration is read.
- **Ending a command.** A command that times out or is cancelled is ended at once together with every process it
  started: Windows has no `SIGTERM` that lets a program clean up first. A `git` command cut off this way can leave
  `.git/index.lock` behind; remove it if a later `git` reports that another git process seems to be running.
- **Agent panes.** The agent starts in its worktree by path, checked against the granted directory by file
  identity before and after the process is created: a worktree renamed or replaced since the extension opened it
  is refused (`… is no longer the one that was opened.`), and so is a path longer than 258 characters, never cut
  short. While the agent runs, the worktree -- and every directory above it -- cannot be renamed, and it cannot be
  removed. The agent and everything it starts, detached helpers and a browser it launches fresh included, run in
  one kill-on-close job: the whole tree ends when the pane is closed or terminated, when the daemon dies, or 5 s
  after its console closes (sooner if the agent exits). A live upgrade carries each agent's job to the new daemon,
  so agents keep running. A user's own shell runs outside any job, so something like `code .` outlives its tab.
- **`file-read`** walks beneath the root's directory handle one component at a time. A junction, symbolic link or
  mount point anywhere on the path, a file with a second name (a hard link), a file on another volume, a
  cloud-files placeholder whose contents are not on this disk (it is never downloaded), or any other special entry
  is refused. Names compare as Windows compares them -- ignoring case, and an 8.3 short name reaches the same
  entry -- and fenced directories are matched by file identity, so no spelling reaches one; a stream (`a:b`), a
  reserved device name or a name ending in a dot or a space is a bad argument. A file another process holds open
  without read sharing is unreadable; one open for writing elsewhere is read, and a read never stops a file from
  being renamed or deleted. A share whose server reports no file identities (some SMB servers) cannot be read
  from, and a stalled share can hold a read: reads are bounded by size, not time.
```

  5. **Orchestrator intro** (~:645): "Contour's `.deb`, `.rpm`, AppImage and macOS `.dmg` packages carry it" →
     "Contour's `.deb`, `.rpm`, AppImage and macOS `.dmg` packages, and the Windows installers and zip (so also
     winget), carry it".
  6. **`programs` row** (~:684): after "an absolute path, or one starting with `~/`" insert "(on Windows also
     `C:\…`, `\\server\share\…` or `~\…`, and an `.exe`)".
  7. **After the configuration's key table** (before "Its `config`:"), add:

```markdown
**On Windows**, write a path in single quotes or unquoted -- `claude: 'C:\Users\me\.local\bin\claude.exe'` -- or
with forward slashes: in double quotes YAML reads `\U` (as in `"C:\Users"`) as an escape and the file does not
load. Claude Code must be its native `claude.exe`: one installed with `npm` is a `claude.cmd` script, which the
host refuses (see [Daemon extensions on Windows](#daemon-extensions-on-windows)). A worktree cannot be removed or
renamed while its agent's tab is open, so ending a run closes the tab first.
```

  8. **Limitations** (~:850): delete the "**Linux and macOS.** …" bullet; add as the last bullet "- **A worktree
     stays while its agent runs, on Windows.** It cannot be removed or renamed until the agent's tab is closed."
  After editing, `grep -n -i "windows" "$S/extensions.ours.md"` and read every hit: no remaining claim that
  Windows lacks `file-read`, `pane-open` or the packaged orchestrator. Check for user data (no real names, hosts).
- [ ] **Step 3: Overlap check — stop if ours touches the user's lines.**

```bash
git show HEAD:docs/extensions.md > "$S/extensions.head.md"
diff -U3 "$S/extensions.head.md" "$S/extensions.ours.md" > "$S/extensions.ours.patch"
# Old-side ranges of every hunk, ours and the user's; any intersection (including the 3 context lines) = STOP.
ranges() { grep -E '^@@' "$1" | sed -E 's/^@@ -([0-9]+)(,([0-9]+))? .*/\1 \3/' | awk '{print $1, $1+($2==""?1:$2)-1}'; }
ranges "$S/extensions.user.patch" > "$S/user.ranges"; ranges "$S/extensions.ours.patch" > "$S/ours.ranges"
awk 'NR==FNR{u[NR]=$0; n=NR; next} {for(i=1;i<=n;i++){split(u[i],r," "); if ($1<=r[2] && r[1]<=$2) {print "OVERLAP", $0, "with", u[i]; bad=1}}} END{exit bad}' \
    "$S/user.ranges" "$S/ours.ranges" || { echo "STOP: report the overlap; do not merge"; exit 1; }
```

  Expected: no output, exit 0 (the user's hunk is around 259–273, ours start past 420).
- [ ] **Step 4: Index only, then commit.**

```bash
blob=$(git hash-object -w "$S/extensions.ours.md")
git update-index --cacheinfo 100644,"$blob",docs/extensions.md
git diff --cached --name-only            # exactly: docs/extensions.md
git cat-file -p :docs/extensions.md | cmp - "$S/extensions.ours.md" && echo "index == ours"
git commit -m "docs(extensions): the orchestrator and daemon extensions on Windows

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

- [ ] **Step 5: Merge ours into the working tree, three-way.**

```bash
sed 's/$/\r/' "$S/extensions.ours.md" > "$S/extensions.ours.crlf"   # ours.md is LF-only; check: ! grep -q $'\r' "$S/extensions.ours.md"
cp docs/extensions.md "$S/extensions.current.crlf"
git merge-file -p "$S/extensions.current.crlf" "$S/extensions.base.crlf" "$S/extensions.ours.crlf" > "$S/extensions.merged.crlf"
echo "merge-file exit: $?"               # must be 0; >0 = conflicts: STOP, leave docs/extensions.md untouched, report
cp "$S/extensions.merged.crlf" docs/extensions.md
```

- [ ] **Step 6: Verify the user's work is exactly as it was.**

```bash
git show HEAD:docs/extensions.md | cmp - "$S/extensions.ours.md" && echo "HEAD == ours"
git diff --cached --quiet && echo "nothing staged"
git diff -- docs/extensions.md | grep -E '^[-+]' | grep -vE '^(\+\+\+|---) ' | diff - "$S/extensions.user.lines" && echo "user hunks unchanged"
git diff --stat -- docs/extensions.md   # the same +/- counts as at the start (6 lines changed: 4+, 2-)
git diff -- cmake/ContourWasiCxx.cmake | sha256sum | diff - "$S/user-cmake.sha" && echo "cmake WIP unchanged"
```

  All five must print their confirmation (the docs fingerprint of Task 1 Step 0 matches too, since the line
  numbers of the user's hunk are unchanged: `git diff -- docs/extensions.md | sha256sum | diff -
  "$S/user-docs.sha"`; if only that differs, compare the two diffs by eye and report). If any fails: `cp "$S/extensions.user-wip.md" docs/extensions.md`
  restores the working tree (the commit stays; it does not touch the working tree), then report.

### Task 11: The smoke check — daemon, orchestrator, GitLab, Jira

**Files:** Create: `scripts/smoke-orchestrator-windows.ps1`

- [ ] **Step 1: Write the script.** Parameters: `-Build` (default `out\build\clangcl-release`), `-Root` (default
  `$env:TEMP\contour-orch-smoke`), `-Tier Offline|Claude|Forge` (default `Offline`), `-GitLabRepo <path>`,
  `-JiraSite <https://…>`, `-Jql <text>` (the last three required for `Forge`, never defaulted), `-Keep`, `-Stop`
  (stop the daemon and clean up, then exit), `-Probe rename|board|tree` (one manual-table check against the running
  smoke, then exit). It must:
  1. **Preflight**: `$Build\bin\contour.exe` and `$Build\wasm\orchestrator.wasm` exist; `git.exe` found
     (`Get-Command git -CommandType Application`); `claude` resolves to an `.exe` (`Get-Command claude
     -CommandType Application | Where-Object Extension -eq '.exe'`), else stop: "install Claude Code's native
     claude.exe". For `Forge`: `glab.exe` and `acli.exe` found, `glab auth status` (run in `-GitLabRepo`) and
     `acli jira auth status` exit 0, and both again with `$env:LOCALAPPDATA` set to `$Root\localappdata` must give
     the same exit codes — otherwise stop ("state isolation would log glab/acli out").
  2. **Install** into `$Root\install` (`cmake --install $Build --prefix $Root\install`), then assert
     `$Root\install\share\contour\extensions\orchestrator.wasm` exists — the bundled lookup the packages rely on.
  3. **Offline repository**: `git init -b main $Root\repos\smoke`, one commit (`README.md`), a bare
     `$Root\remote\smoke.git` as `origin` (a `file` remote, which the manifest's `GIT_ALLOW_PROTOCOL` allows),
     `git push -u origin main`.
  4. **Configuration** `$Root\contour.yml`, written with single-quoted Windows paths (the documented spelling):

```yaml
daemon:
    extensions:
        orchestrator:
            autostart: true
            programs:
                claude: '<claude.exe found in 1>'
                smoke-not-exe: '<Root>\bin\not-an-exe.cmd'    # Task 2's warning; the orchestrator never runs it
            config:
                worktree_root: '<Root>\wt'
                models: [sonnet]
                status_board_after: 1m
                end_done_after: 0
                permissions: ask
                repos:
                    - { path: '<Root>\repos\smoke', name: smoke, forge: gitlab, base: main }
                    # Forge tier only:
                    - path: '<GitLabRepo>'
                      forge: gitlab
                      issues:
                          - { source: jira, jql: '<Jql>', site: '<JiraSite>' }
                tasks:
                    - { id: topic, key: t, label: topic, applies_to: topic, model: sonnet,
                        prompt: "Write the word smoke into STATUS.md at the worktree's root, then stop." }
                    - { id: issue, key: w, label: "work on issue", applies_to: issue, model: sonnet,
                        prompt: "Read {url} and write a one-line plan into STATUS.md, then stop." }
```

  5. **Start**: with `$env:LOCALAPPDATA = "$Root\localappdata"` for this process (restored on exit),
     `& $Root\install\bin\contour.exe daemon --name orch-smoke --config $Root\contour.yml --background
     --log-file $Root\daemon.log` (check `contour daemon --help` for the flag spellings first).
  6. **Automated checks**, printing PASS/FAIL per line:
     - `daemon.log` contains `Program 'smoke-not-exe' of extension 'orchestrator'` and `only an .exe` (Task 2);
     - `contour extension list --daemon orch-smoke` shows `orchestrator` `pending`, its question names
       `…\contour-orch-smoke\wt` and `…\repos\smoke`, the paths contain no `~1` even though `$env:TEMP` is an 8.3
       spelling on this machine (A2's long-name resolution), and the module line ends in
       `share\contour\extensions\orchestrator.wasm`.
  7. **Print the manual steps and the expected observations** (below), then poll `contour extension status
     orchestrator --daemon orch-smoke` until `running` (5 min limit) after the user grants.
  8. **`-Stop`**: `contour daemon stop --name orch-smoke` (add the flag that skips its question, if it asks);
     `git -C <each repo> worktree prune`; list the `agent/*` branches the run left in `-GitLabRepo` for the user to
     delete (never delete anything in the user's repository); remove `$Root` unless `-Keep`.
  Nothing the script writes goes anywhere but `$Root`, the daemon's own state under the overridden
  `LOCALAPPDATA`, and (Forge tier) the worktree metadata git keeps in `-GitLabRepo\.git\worktrees`.
- [ ] **Step 2: The manual part** (printed by the script, and recorded in Task 12):

  | # | Do | Expect | Tier |
  | --- | --- | --- | --- |
  | 1 | With the same `LOCALAPPDATA`, `contour.exe client --daemon orch-smoke` (the script starts it) | The window asks "“orchestrator” wants to run in the background", directories in `C:\…` long spelling; grant | Offline |
  | 2 | Command palette → "Open Agents" | The `orchestrator://` tab; under problems, `smoke`'s `glab` checks fail (its remote is no GitLab host) with a readable reason — any numeric code decimal or `0x…`, never a negative number | Offline |
  | 3 | `n`, type a topic, choose `topic`, confirm | A worktree appears under `$Root\wt\smoke\<run>`; a tab runs `claude.exe` in it (without a Claude login it shows its login screen — the pane, its job and the directory are what is tested) | Offline |
  | 4 | Script check `-Probe rename`: `Rename-Item` the worktree and its parent `smoke` | Both refused (access denied) while the tab is open | Offline |
  | 5 | Write `STATUS.md` into the worktree by hand (`-Probe board`), press `b` | The board shows its text (`file-read` on Windows) | Offline |
  | 6 | `-Probe tree`: list `claude.exe` and its children (`Get-CimInstance Win32_Process` by parent) | PIDs recorded | Offline |
  | 7 | `contour daemon upgrade --name orch-smoke` | The tab and its `claude.exe` PID survive; the dashboard still lists the run | Offline |
  | 8 | Delete `STATUS.md`, press `e` on the run | The tab closes first, then the worktree is removed (`git worktree list` no longer names it); every PID from 6 is gone within 5 s | Offline |
  | 9 | Logged in to Claude: repeat 3 and let the agent finish | The agent writes `STATUS.md`; the board shows it; note whether a refresh ever answered "unreadable" (Claude Code writing by rename) | Claude |
  | 10 | `t` on that run | The transcript pages load: `contour agent transcript` matched the session log's Windows `cwd` spelling | Claude |
  | 11 | With `-Tier Forge` | The MR list has as many rows as `glab mr list --author=@me --per-page 100 --output json` returns in `-GitLabRepo` (the script prints the count only), the issue list as many as `acli jira workitem search --jql <Jql> --limit 100 --json`; CI marks follow `detailed_merge_status` | Forge |
  | 12 | Start `issue` on one Jira item (spends Claude tokens; optional) | Worktree from `base`/the default branch, the agent's prompt carries `<JiraSite>/browse/<KEY>` | Forge |

- [ ] **Step 3: Run it** (`pwsh -NoProfile -File scripts/smoke-orchestrator-windows.ps1`), as far as this machine
  allows: the Offline tier fully (rows 1–8 need the user at the window for the grant and the keys — hand those rows
  to the user if running unattended); Claude and Forge only with the user present, their own accounts, never with
  values written into the repository. Record PASS/FAIL per row, without personal data, for Task 12.
- [ ] **Step 4:** `Invoke-ScriptAnalyzer scripts/smoke-orchestrator-windows.ps1` if PSScriptAnalyzer is installed;
  `git grep -n -i -E "<the user's name>|<home dir>|atlassian.net|gitlab\." -- scripts/smoke-orchestrator-windows.ps1`
  → only the `example` placeholders.
- [ ] **Step 5: Commit** the script: `test(orchestrator): a Windows smoke check of the daemon, GitLab and Jira`.

### Task 12: The roadmap — every phase done

**Files:** Modify: `docs/superpowers/plans/2026-11-23-orchestrator-windows-glab-acli-roadmap.md` (no user WIP in
it; `git diff --quiet -- <it>` before editing — if it has uncommitted changes, apply the Task 10 procedure instead)

- [ ] **Step 1:** Status table: every row "**done**" with its plan's file name and the commits' one-line summary;
  row 1 "recorded and enabled"; row E "**done**: the MSI, per-user MSI and zip carry
  `share/contour/extensions/orchestrator.wasm` (plan `2026-11-24-orchestrator-windows-packaging-and-docs.md`)";
  "Blocks" `—` throughout. Phases list: mark A2, B, C, D, E done.
- [ ] **Step 2:** "Pre-existing Windows test failures": replace with Task 1's measured list (each with file:line and
  whether it is environmental); drop the entries Task 1 showed fixed, saying by which phase.
- [ ] **Step 3:** "Known Windows differences": keep the rename entry, add one line each (pointing at
  `docs/extensions.md#daemon-extensions-on-windows`): `.exe`-only programs and npm's `claude.cmd`; no graceful
  stop (timeouts end the tree, `index.lock`); a held directory is entered by path, renamed ⇒ refused, > 258
  characters refused; the worktree cannot be removed or renamed while its agent runs; job lifetime (pane close,
  daemon death, 5 s console grace) and the live upgrade carrying jobs; user shells outside any job; `..` refused
  in roots rather than collapsed; SMB shares without file ids unreadable; YAML double-quoted `C:\U…`.
- [ ] **Step 4:** Add "## End-to-end run (2026-11-24)": the smoke script's command, the tier run, and Task 11's
  PASS/FAIL per row — no user names, hosts, sites, JQL or keys.
- [ ] **Step 5: Commit** the roadmap: `docs(orchestrator): the Windows roadmap is done`.

### Final

- [ ] Run `/simplify` over the branch's phase-E commits; then build everything
  (`cmake --build --preset clangcl-release`) and run `ctest --test-dir out/build/clangcl-release -j 8` → no failure
  beyond Task 1's list. clang-format every touched C++ file.
- [ ] Re-run Task 10 Step 6's verification — the user's hunks in both files are untouched at the end of the plan.
- [ ] Summary per AGENT.md: performance impact (none at run time; CI +download/+guest build on both Windows jobs),
  risk (CI wiring on first run; Task 5/6 change Windows spawn/handover paths), coverage (new tests per task).

## Out of scope

- Moving `CONTOUR_EXTENSIONS_ORCHESTRATOR_GUEST` into the `msvc-release` preset (Decision 1) — after the user's
  CPM fetch in `ContourWasiCxx.cmake` is committed.
- `Process::workingDirectory()` answering `"."` on Windows; a graceful console-close-first `terminate()` for
  `claude`; dropping the inherited `=C:` drive variables from held spawns (pane-open plan, "gaps to raise").
- A path-based `file-read` fallback for SMB shares without file ids.
