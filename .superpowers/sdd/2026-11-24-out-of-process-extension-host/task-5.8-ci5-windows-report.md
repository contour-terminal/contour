# 5.8 CI round 5 (Windows): report

**Base:** c66b53a20 (local, unpushed). **Head:** c47cf3330. Not pushed.

## Commits

| Commit | Change |
|---|---|
| `ca215d8f8` (was 8538f54dc, the contained-environment fix) | items 1 and 2 folded in |
| `73d8a659c` (was b0686b841, "the extension host names every descriptor it keeps") | item 3 folded in |
| `c47cf3330` (new, on top) | item 4: fix(vtextension): a channel end on Windows waits on events of its own, made once |

How the history was rebuilt:

- The fixups were applied by autosquash from the merge base. There were no conflicts.
- The rebased tree is byte-identical to the tree before the rebase.
- c66b53a20's patch (now `a2ee08834`) is unchanged.
- The other macOS commits differ only in hunk offsets and blob hashes.
- The dependencies of the folded code exist at those commits: `core::views::enumerate` and `WakeStopHandles` at `73d8a659c`, `Wine.hpp` at `ca215d8f8`.

Item 4 is a new commit instead of a fold for two reasons. Its owner (070f35fa5) predates `wasm/testing/Wine.hpp`, which the new test's wine skip needs. And twelve later commits touch the owner's files.

## 1. Container folder: the system doubles a container folder it is handed

**The brief's reading was not the cause.** The brief read the second line under `value=` as the expected
folder, printed relative. That line is in fact Catch2 wrapping the probe's own **value**. The full report,
from the `[appcontainer]` re-run (log ~35400), is:

```
value-LOCALAPPDATA=C:\Users\runneradmin\AppData\Local\Packages\contour.extensionhost\AC\Packages\contour.extensionhost\AC
value-TEMP=…\AC\Packages\contour.extensionhost\AC\Temp
value-TMP=…\AC\Packages\contour.extensionhost\AC\Temp
```

**What Windows did.** We handed CreateProcessW `LOCALAPPDATA` = the container folder. Windows extended it to
`<given>\Packages\<moniker>\AC`, and derived `TEMP` and `TMP` from the `LOCALAPPDATA` it was given as well. Our
TEMP and TMP values were overwritten. This is the doubling flagged as a risk in ci2 round 1.

**The fix keeps the comparison exact and fixes the input:**

- `ContainerVariable` gains a `ContainerVariableSource` (`ProfileFolder`, `ContainerFolder`). `LOCALAPPDATA`
  is handed the profile's local folder, which Windows extends to the container's. `TEMP` and `TMP` are handed
  the values the child is to see; Windows replaces them anyway, but they must be present (error 203
  otherwise).
- New `ContainerFolders { container, profile }` and `containerFoldersOf(folder)`. The latter accepts only
  `<profile>\Packages\<name>\AC` (names compared ignoring ASCII case, a trailing separator tolerated) and
  yields the profile folder.
- `AppContainerProvision::prepare` refuses any other shape at `LaunchStep::ContainerFolder` with
  `HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME)`. `PreparedContainer` carries `ContainerFolders`, and the launcher
  hands them to `containedEnvironment`.

**Tests:**

- `LaunchSpelling.containerFoldersAreReadFromTheContainersFolder`: the shape, case, a trailing separator, and
  five refused shapes.
- `LaunchSpelling.containedEnvironmentNamesTheContainersFolders` and
  `LaunchSpelling.helperEnvironmentContainedIsOneBlock`: updated to the profile-folder input.
- `AppContainer.profileIsEnsuredOnce`: the profile folder is checked, and the empty-folder section became a
  table that adds two wrong shapes (ERROR_BAD_PATHNAME).

The CI check (`checkContainerVariables`, exact apart from case and one trailing separator) is unchanged. It
compares with the container folder `expectedContainerFolder()` derives, which was already correct.

## 2. The "missing" spawn line: present, with a different code

**The brief's reading was not the cause here either.** The `spawn` line was present:
`spawn=refused:CreateProcessW:367`. That is ERROR_CHILD_PROCESS_BLOCKED, the code the child process policy
(`PROCESS_CREATION_CHILD_PROCESS_RESTRICTED`) gives. The test expected 5 (ERROR_ACCESS_DENIED). `valueOf()`
returns an `optional`, which Catch printed as `{?}`, so the actual value was hidden. The report is sent as one
block after every row is computed, so a slow connect cannot cut rows off.

**Fixes:**

- The test expects `refused:CreateProcessW:367`, and the case's comment names ERROR_CHILD_PROCESS_BLOCKED and
  the runner that showed it.
- Every `CHECK(valueOf(report, …) == …)` now compares `.value_or("(no line)")`, so a failure prints the actual
  value or "(no line)". A missing row still fails loudly.
- **The 21 s connect.** The blocking connect waited out WSAETIMEDOUT (10060), which took about 20 s. The probe
  now connects non-blocking (`FIONBIO`) and waits with `select()` for `ConnectPatience` (2 s). If no answer
  comes, it reports `refused:connect:timeout`, which is added to `ContainedConnectRefusals`; 10060 stays
  accepted. Uncontained, the loopback connect is immediate (`ok`, checked).

## 3. StopReceiver and StopSender report the same handle

**This is correct on Windows, and the held list was wrong.** `WakeReceiver::stopHandles()` returns one event
in both fields on Windows (`WakeSignal_win32.cpp`: `{ .waitedOn = _stopEvent, .signalled = _stopEvent }`). It
is documented in `WakeStopHandles`: "on Windows one event, which both name". `sandboxDescriptorsOf()` then
recorded that one handle twice, under two roles.

**Fixes:**

- New `DescriptorRole::StopEvent`: "one handle is both what its wait waits on and what stop() signals
  (Windows)", with rights `{Read, Write, Event}` in the data table. The table's static_asserts are updated.
- `sandboxDescriptorsOf()` records one `StopEvent` when the two handles are the same, and `StopReceiver` plus
  `StopSender` otherwise. No handle is recorded twice.
- **Tests:**
  - `HostProcess_test` checks the held list against `WakeStopRoles`: `{StopEvent}` on Windows,
    `{StopReceiver, StopSender}` on POSIX. It also checks that no stop handle is held twice. This covers all
    seven cases that go through the helper.
  - `Sandbox_test` covers the new role's rights.

## 4. One handle more while 1000 instances live

**Analysis.** The count was 124 against 123 only while the instances were alive. After `clear()` it matched
again, so nothing outlived the instances, and the extra handle was not per-instance (1000 would be +1000).

The source is in `Channel_win32.cpp`: every transfer (`transferCounted`) made an `OverlappedEvent` and closed
it when it returned. A thread blocked in a receive therefore holds one handle more than the same thread between
receives. The in-process host's control thread goes back into its receive after it answers. So whether that
handle exists when the test counts depends on whether the host thread has re-entered `receive()` after its
last reply. That is a race against the baseline, not a leak.

**Fix (`c47cf3330`).** Each channel end makes one event per direction, `sendEvent` and `receiveEvent`, with
itself and reuses it.

- `TransferDirection` selects the event.
- Channel.hpp already allows at most one sender and one receiver per end at a time, so one event per direction
  is safe.
- ReadFile and WriteFile reset the event as they start.
- An end now holds the same handles whether a transfer is in flight or not.
- It also saves a CreateEvent and a CloseHandle per transfer.

**Test.** New `Channel.transferInFlightOpensNoHandle`: with a receive blocked for 200 ms, the process's handle
count never exceeds the count before.

**The limit of the evidence.** Wine cannot verify any of this. Its `GetProcessHandleCount()` reads **0**
whatever the process holds; I measured that with an extra event. So under wine the original test compared
0 == 0, and my new test passed against the old code too. Both handle-count cases now SKIP under wine with that
reason, rather than passing vacuously. The cause is established by reading the code and by the timing pattern,
not by a measurement. The next Windows CI run decides.

## What else runs under wine (surfacing failures hidden by stopOnFailure)

I cross-built the full suites, including `vtextension_remote_test`, which needed the MinGW `zlib1.dll` copied
next to it locally (not committed), and `vtextension_test`. All ran under wine 11:

| Suite | Result |
|---|---|
| `vtextension_test` | 425 cases, 419 passed, 6 skipped, 24965 assertions |
| `vtextension_remote_test` | 273 cases, 270 passed, 3 skipped (the handle-count case among them), 11517 assertions |
| `vtextension_process_test` | 130 cases, 127 passed, 3 skipped, 1749 assertions |
| `vtextension_remote_wamr_test` | 53 cases, 633 assertions, all passed |
| `vtextension_wasmtime_test` | 65 cases, 64 passed, 1 skipped |
| `vtextension_wamr_helper_test "~[.measure]"` | 53 cases, 52 passed, 1 skipped |
| `[appcontainer]` in `vtextension_process_test` | 7 cases, 6 passed, 1 skipped |
| `[appcontainer]` in `vtextension_wamr_helper_test` | 6 cases, 1502 assertions, all passed |

Under wine, `HostProcess.*` (17 cases) passes after the fix. `RemoteBackend.instancesHoldNoDescriptor` passed
before only because wine's count is 0.

**What I could not run:**

- The contained halves. Wine stubs AppContainers, so these skip as before.
- Any real handle count.
- `vtextension_helper_test`, which is not configured in this WAMR cross tree.

## Other verification

- **Linux ASan:** the vtextension suites and the probe built warning-free.
  - `ctest -R "vtextension|apply_patch_once"`: 8 of 8 passed.
  - `ctest -L lint`: 35 of 35 passed.
- **clang-tidy diff:** Linux 8 TUs clean, Windows 13 TUs clean. Three findings were fixed: a redundant cast in
  the probe, a moved-from `future` read inside `CHECK` (now hoisted), and a by-value lambda parameter.
- **Format:** clang-format 22 and editorconfig-checker are clean.

## Concerns

- **Item 4 is unverified.** It rests on code reading. If the count still differs on Windows, the next suspect
  is a transient system thread-pool handle, which would need the test to count only handle types the code owns.
- **Item 1 depends on Windows' current behaviour.** The fix relies on CreateProcessW extending `LOCALAPPDATA` as
  observed (`<given>\Packages\<moniker>\AC`). The exact CI check will show whether that holds.
- **Item 2 deviates from the folding rule.** It is folded into the contained-environment commit rather than into
  the much older commits that first wrote the probe's connect and the spawn expectation: that commit already
  owns most of those lines, and a deeper fold would conflict.

## Round 1 (review fixes)

All eight findings are fixed and folded by fixup and autosquash. The rebased tree is identical to the fixup
head, and c66b53a20's patch is unchanged. **The new head is `d403214fa`.**

**The channel commit** (`c47cf3330`, now `d403214fa`) takes findings 1, 2, 3, 4 and 7:

1. **Important: the test's baseline.** `Channel.transferInFlightOpensNoHandle` now starts its receiving thread
   first, parked on a `std::promise`. It takes the baseline count only after that, then releases the thread
   into `receiveSome`. A thread's own handles are thus in the baseline.
2. **Important: event-creation failure is refused, not swallowed.** The events are made by `makeEndEvents()`,
   which returns `std::expected<EndEvents, ChannelError>`. `OverlappedEvent` became movable, and
   `Native(HANDLE, EndEvents)` takes the events. Both `Channel::adopt()` and `makeChannelPair()` refuse with
   `Io` when either event cannot be made. `adopt()` makes the events before it touches the handle, so a refused
   handle stays the caller's. The per-transfer null check, which used to swallow the failure, is gone.
3. **Channel.hpp's contract** now says one thread at a time receives on an end, as one sends, and that on
   Windows each direction of an end waits on one event of its own. The `TransferDirection` comment says "and
   never two of either".
4. **The `OverlappedEvent` comment** now reads "a manual-reset one, closed when this is destroyed". The new
   `EndEvents` is documented as made with the end and closed with it.
7. **The held-list contract is narrowed, not extended.** `sandboxDescriptorsOf()` in HostProcess.hpp now
   says it lists the handles a sandbox limiting handle by handle (FreeBSD's) confines. Not every handle on
   Windows, where each channel end also keeps two events of its own that no role names.

   Naming the events would have meant a Windows-only accessor on `Channel`, a new role, and test expectations
   for host-internal handles. That serves no sandbox, since nothing on Windows confines by role. So I narrowed
   the wording, as the finding allowed.

**The descriptor commit** (`73d8a659c`) takes finding 8:

8. **`StopEventRights`.** Its comment says the row is Windows' alone, where no sandbox limits a handle by its
   role: it names the handle's use, and nothing enforces it.

**The contained-environment commit** (`ca215d8f8`) takes findings 5 and 6:

5. **`containerFoldersOf()`** now also requires `profile.is_absolute()`, and the header says so. Two refused
   rows are added: `Local\Packages\c\AC`, and `\Local\Packages\c\AC`, which is rooted but has no drive.
6. **The probe checks `getsockopt()`.** A failure now reports `refused:getsockopt:<WSA error>` instead of
   reading an unset error.

**Tests:**

- **Windows cross build:** `vtextension_test`, `vtextension_process_test`, both probes,
  `vtextension_wamr_helper_test` and `vtextension_remote_test`. Exit 0, no warnings.
- **Under wine:**

  | Run | Result |
  |---|---|
  | `vtextension_test` | 425 cases, 419 passed, 6 skipped |
  | `vtextension_process_test` | 130 cases, 127 passed, 3 skipped, 1751 assertions |
  | `vtextension_remote_test` | 273 cases, 270 passed, 3 skipped |
  | `[appcontainer]` in `vtextension_process_test` | 7 cases, 6 passed, 1 skipped |
  | `[appcontainer]` in `vtextension_wamr_helper_test` | 6 cases, all passed |

  The handle-count cases still skip under wine, whose count reads 0.
- **Linux ASan:** `vtextension_test`, `vtextension_process_test`, `vtextension_remote_test` and
  `vtextension_helper_test`: 4 of 4 passed. `ctest -L lint`: 35 of 35 passed.
- **clang-tidy diff:** Linux 5 TUs clean, Windows 8 TUs clean.
- **Format:** clang-format 22 and editorconfig-checker are clean.

**Concerns:** as before, item 4's cause and its fix are verified only by the next Windows run, because wine
counts no handles.

## Round 2 (re-review fixes)

Both items are amended into the channel commit, which was the head commit (`d403214fa`) and is now
**`9c87b6d5d`**. The other commits are unchanged.

1. **The baseline waits until the thread is running.** `Channel.transferInFlightOpensNoHandle` adds a second
   promise, `started`. The receiving thread sets it before it parks on `go`, and the test waits on it before
   taking the baseline. Its future is taken before the thread starts, so the two never race on the promise.
   std::async's pool may create the thread's handles only once it runs; those are now inside the baseline.
2. **No hang on a failed REQUIRE.** A `core::Finally` declared after the `received` future is destroyed
   before it. If the test has not released the thread, it sets `go` (tracked by `goSet`), then shuts the
   helper end down. Whatever fails first, the thread is released and its receive returns `Closed`, so the
   future's destructor never waits on a receive nothing would end.

**Tests:**

- The Windows cross build of `vtextension_test` is clean, with no warnings.
- `vtextension_test` under wine: 425 cases, 419 passed, 6 skipped. The case itself skips under wine, whose
  handle count reads 0.
- Linux ASan `vtextension_test`: passed.
- clang-tidy diff (Windows tree, 1 TU) and clang-format 22 are clean.
