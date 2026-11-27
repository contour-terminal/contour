# Task 3.5 report: teardown off the owner thread

Status: DONE_WITH_CONCERNS. The concerns are decisions the brief did not make; they are listed under Deviations. Two of them, D1 and D2, change what the brief's test scenario meant.

## Commits (on 3e3cd46bd)

### 97967f735 `build(vtextension): run every helper case, and ask for our stderr only when needed` (item 0)

- Removed the dead `~[in-process]` CTest filter for `vtextension_helper_test`, and the comment that described it. No case carries the tag since 3.4's fix round.
- `Process_win32.cpp`: `GetStdHandle(STD_ERROR_HANDLE)` was passed to `optional::value_or`, which evaluated it eagerly. It is now called only when the spec names no handle.

### f3899d673 `feat(vtextension): let go of an extension host without waiting for it`

## What changed (paths under `src/`)

### Transport

- `vtextension/wasm/remote/transport/Channel.hpp` gains `Channel::trySend(span)` and returns how many bytes were sent.
  - `Channel_posix.cpp`: `send(MSG_DONTWAIT|SendFlags)` in a loop; EAGAIN stops it.
  - `Channel_win32.cpp`:
    - `Native::transfer` now takes a `Completion { Poll, Wait }`.
    - Poll starts an overlapped `WriteFile` and calls `GetOverlappedResult(FALSE)`.
    - If the write is still pending (`ERROR_IO_INCOMPLETE`), it calls `CancelIoEx`, then `GetOverlappedResult(TRUE)`, and reports the bytes moved.
    - The shutdown and in-flight counting are unchanged.
- `PduStream.hpp/.cpp`:
  - New `SendOutcome { WouldBlock, Sent }` and `PduStream::trySend(pdu)`.
  - `_unsentTail` holds the rest of a frame that was begun. `send()` and `trySend()` both send it first. While it is not drained, `trySend` returns `WouldBlock`.
  - `Sent` means the frame is the stream's from now on: either all of it was sent, or its tail is kept.
  - Test hook `unsentBytes()`.
  - Private `frameOf()` holds the shared encode and over-cap check.

### Parent side

- `HostConnector.hpp`: `IHostHandle::letGo()`. It lets the host go now, as destroying the handle would, while the handle stays usable to the threads that hold it.
- `HelperIncarnation.hpp/.cpp`:
  - `MaxPendingReleases = 4096`.
  - `post()` is replaced by `release(proto::Release)` (D51):
    1. Push onto `_outbox` under `_outboxMutex`, which is never held across I/O.
    2. `std::unique_lock{_controlMutex, try_to_lock}`.
    3. `trySendReleases()`, front to back, until `WouldBlock`.
    - More than 4096 queued → `markDead()`.
  - `request()` calls `sendReleases()` (blocking) before its exchange, and again after it, under the step's watch.
  - New `letGo()`:
    - The state exchange makes it mutually exclusive with `markDead()`.
    - It calls `_handle->letGo()`, then shuts every channel down.
  - `shutdownChannels()` is factored out.
- `RemoteBackend.cpp`:
  - `~RemoteModule`, `~RemoteInstance` and the module re-adoption path call `release()`.
  - The `~RemoteInstance` comment now explains why the slot ordering still holds: the next Instantiate is a request, and a request sends the queued releases first.
  - New `IncarnationRoster`:
    - The backend owns it, and passes it on to each `RemoteRuntime` and from there to each `HelperSlot`.
    - Each incarnation is listed with a `weak_ptr`.
    - `~RemoteBackend()` calls `close()`, which runs `letGo()` on every listed incarnation.
    - After that, `HelperSlot::live()` refuses with `HostUnavailable` before starting anything. An incarnation that connected during the close is let go at once.
  - The `RemoteBackend` constructor is no longer `noexcept`, because it allocates the roster. Copy and move are deleted.
- `RemoteBackend.hpp`: the doc now says that nothing a backend lets go waits for a host.

### Supervisor

- `process/ExtensionHostSupervisor.hpp/.cpp`:
  - `SupervisedHelper::_key` is `std::atomic<uint64_t>`.
  - `release()` became the public `letGo()`, an exchange to 0, so there is only ever one Drop.
  - `kill`, `watch` and `attach` load the key once.
  - Grace, Drop and Stop were already right in 3.3 and are unchanged.
- `process/ProcessHostConnector.cpp`: `letGo()` → `_helper.letGo()`.
- `host/InProcessHost.hpp/.cpp`: `letGo()` → `terminate()`. The destructor still joins (D17/D51, tests only).

### Test executables

- `contour-extension-host/testing/HostileModes.hpp`, `HostileHost.cpp`:
  - Row `stop-reading-after-instantiate`: a `StoppingRelay` sits on the control channel only.
    - Contour→host traffic is relayed frame by frame (`frameSizeOf`).
    - Host→Contour traffic is decoded with `decodeFrame`, and the relay watches for `Instantiated`.
    - Once an `Instantiated` has been forwarded, the relay stops reading for good at the header of the next `Load`.
  - Row `ignore-eof`: runs the real host. If it returned 0 (Contour's end of stream), it ignores SIGTERM and sleeps until killed. Any other exit code is returned as is.
- `process/testing/LaunchProbe.cpp`: new mode `--parent <helper arguments> <helper> [extra…]`.
  - It binds itself to its parent, then starts a supervisor over a `RecordingLauncher` and a `ProcessHostConnector`.
  - It waits for the helper's Hello, then sends `helper=<pid>\nend\n` on its control channel, and waits to be killed or for end of stream.
  - The probe now links `vtextension_remote_process`.
- `process/testing/HelperComposition.hpp/.cpp`:
  - The constructor takes a `teardownGrace` (default `HelperTeardownGrace`), which `awaitLiveHelpersAtMost` uses.
  - New `stopSupervisor()`.
- `process/testing/RecordingLauncher.hpp` gains `exitStatuses()`: each process's status is recorded once, on its first reap.
- `remote/CMakeLists.txt`:
  - `Teardown_test.cpp` is added to `vtextension_helper_test`, together with `VTEXTENSION_PROBE_PATH` and a dependency on `extension_host_probe`.
  - The probe's link changed as above.

## Tests (names as the brief asks)

- **Transport** (`vtextension_test`): `Channel.trySendNeverBlocks` and `PduStream.trySendKeepsAPartialFrame`.
  - The second uses 256 KiB frames, so that a socket takes part of one. On POSIX the case checks that a tail was kept.
- **Outbox** (`vtextension_remote_test`, scripted helper):
  - `RemoteBackend.releaseWaitsForTheControlChannel`: the drop returns within 100 ms. The Release is read after the reply to the second Instantiate, with no further request needed to carry it (this checks the drain after the exchange). It also comes before the next request.
  - `RemoteBackend.releaseOverflowKillsTheHelper`:
    - 4097 drops, all on one thread, with the whole loop bounded at 5 s; the slowest drop must take under 100 ms.
    - Before the 4097th drop: 0 terminations. After it: exactly 1.
    - The outstanding Instantiate then fails `HostUnavailable`.
  - Extra: `RemoteBackend.droppedBackendLetsItsHostsGo`:
    - The owner drops the instance, module, runtime and backend while another thread's request is outstanding; the drops return within 100 ms.
    - `letGo` 1, terminate 0. The request fails `HostUnavailable`.
    - A straggler's later load fails `HostUnavailable`, and no second host is connected.
- **Supervisor** (`vtextension_process_test`):
  - `Supervisor.teardownGraceLetsAHelperExit` (exits `{Exited,0}` at +1 s; Dropped 0, unclean 0).
  - `Supervisor.teardownGraceThenKills` and `Supervisor.zeroGraceKillsAtOnce`: these are 3.3's `dropWaitsTheTeardownGrace` and `dropKillsAndReaps`, renamed rather than duplicated.
  - Extra: `Supervisor.letGoIsOneDrop`. `letGo`, then the holder's `kill`/`watch` and the destructor, give one Dropped kill at the original grace's end.
- **Review Focus** (`vtextension_helper_test`, `Teardown_test.cpp`):
  - `"teardown never blocks the owner thread"`:
    - Section "closing": the hostile relay, grace 0, `load` timeout 30 s, three instances; W loads a 16 MiB module under identity "same".
      - W is still blocked after 200 ms.
      - The owner's drop of everything runs in `std::async` and must be ready within 1 s.
      - W returns `HostUnavailable` within 5 s.
      - Dropped == 1, reaped == 1, unclean == 0, and every recorded `kill()` ran on `supervisor().threadId()`.
    - Section "quitting": a helper running `ignore-eof`, and on POSIX a real helper that was `SIGSTOP`ped.
      - Their channels are closed while the handles are still held.
      - `stopSupervisor()` runs in `std::async` and must be ready within 1 s.
      - Every kill ran on the supervisor thread, one per helper.
      - Both processes are gone: POSIX `waitpid(WNOHANG)`, or `kill(pid,0)` returning ESRCH, within 2 s; Windows a SYNCHRONIZE handle.
  - `"no helper outlives its parent"`:
    - Linux uses `prctl(PR_SET_CHILD_SUBREAPER)` and FreeBSD `procctl(PROC_REAP_ACQUIRE)`, once per binary.
    - The probe is started with `--parent` through the system launcher, killed, and reaped.
    - `DYNAMIC_SECTION` "benign" / "ignore-eof": the helper must be gone within 2 s. Linux and FreeBSD check with `waitpid` as its subreaper, Windows with a SYNCHRONIZE handle, and macOS by polling `kill(pid,0)` for "benign" only (`SKIP` for "ignore-eof").
    - If a section fails, it kills and reaps the helper itself.
  - `"teardown in coverage builds lets the helper exit"`: grace 2 s; the backend is dropped. `recording().exitStatuses() == { {Exited, 0} }`, Dropped 0, unclean 0.
- Every wait is bounded: `within`/`abandon` (`FAIL_CHECK` plus `abort`) wherever a future's destructor would otherwise wait. Gated scripted hosts wait for their gate at most 10 s.

## Mutation evidence

Each mutation was applied, the binary rebuilt, the named case run under `timeout 60`, and the source restored.

| # | Mutation | Result |
|---|---|---|
| M1 | `trySend` without `MSG_DONTWAIT` | `Channel.trySendNeverBlocks` aborts: "trySend() blocked" |
| M2 | `trySend` keeps no tail | `PduStream.trySendKeepsAPartialFrame` fails (2 assertions; the reader's decode breaks) |
| M3 | `send()` skips the tail | same case fails (3 assertions) |
| M4 | `release()` locks the control mutex blocking | `releaseWaitsForTheControlChannel` and `droppedBackendLetsItsHostsGo` abort at 100 ms ("…waited…"). `releaseOverflowKillsTheHelper` first hung, because its drop loop ran on the test thread; that loop is now in `std::async` with a bound. Re-run with M4, it aborts at 5 s ("dropping the modules waited for the host") |
| M5 | no drain after the exchange | `releaseWaitsForTheControlChannel` fails: the Release is not seen without another request |
| M6 | no overflow check | `releaseOverflowKillsTheHelper` fails (terminations 0; the outstanding request never ends; bounded abort) |
| M7 | `~RemoteBackend` does not close the roster | `droppedBackendLetsItsHostsGo` aborts; "closing" aborts with "the worker's load did not end with its helper" |
| M8 | `SupervisedHelper::letGo` loads instead of exchanging (the destructor drops a second time) | `Supervisor.letGoIsOneDrop` fails (the grace is re-armed and the kill comes late) |
| M9 | no `PR_SET_PDEATHSIG` in the helper | "no helper outlives its parent / ignore-eof" fails; "benign" still passes, by EOF, as designed |
| M10 | `stop()` sends no kills | "quitting" fails (no kills recorded; the processes linger and the case kills them) |
| M11 | Drop ignores the teardown grace | "teardown in coverage builds…" fails: the exit status is a kill's, and Dropped == 1 |

## Results (real counts)

### ASan (`out/clang-asan`), after the final formatting

| Run | Result |
|---|---|
| `vtextension_test` | 397 cases / 13057 assertions |
| `vtextension_remote_test` | 214 cases / 10746 assertions |
| `vtextension_process_test` | 77 cases, 76 passed, 1 skipped (caps); 514 assertions |
| `vtextension_helper_test` | 145 cases, 144 passed, 1 skipped; 2308 assertions; exit 0, so no run-gate failure |
| The three Review Focus cases, 10 consecutive runs | 10/10 green (52 assertions each) |
| `ctest --preset=clang-asan -R "vtextension\|check_wasm_runtime_containment"` | 7/7 passed |

No leftover helper or probe processes (`ps` count 0).

### TSan (`out/clang-tsan`)

All targets above were rebuilt, plus the helper, the hostile helper and the probe. 0 ThreadSanitizer warnings:

| Run | Result |
|---|---|
| `vtextension_test "Channel.*,PduStream.*"` | 18 cases / 94 assertions |
| `vtextension_process_test` | 77 cases (76 passed, 1 skipped) |
| `vtextension_remote_test "RemoteBackend.*,HostServer.*,HostProcess.*,InProcessHost.*"` | 75 cases / 7781 assertions |
| `vtextension_helper_test`, 2 full runs | 145 cases each (144 passed, 1 skipped) |

### Windows (clang-cl cross, `out/clangcl-debug`, -Werror)

`vtextension_process_test`, `extension_host_probe`, `contour-extension-host`, `contour-extension-host-hostile`, `vtextension_helper_test` and `vtextension_remote_test` build clean; that includes item 0's `Process_win32.cpp`.

Under wine, with my own prefix (`scratchpad/wine35`) and `zlib1.dll` from the mingw sysroot on `WINEPATH`:

| Run | Result |
|---|---|
| `vtextension_process_test.exe` | 88 cases / 557 assertions (with the temporary transport tests below) |
| `vtextension_helper_test.exe`, 3 runs | 117 cases, 115 passed, 2 skipped; 1507 assertions each. The teardown cases ran: closing, quitting, benign, ignore-eof (47 assertions) |
| `vtextension_remote_test.exe "RemoteBackend.release*,RemoteBackend.droppedBackendLetsItsHostsGo"` | 4 cases / 5158 assertions |
| Transport tests, through the temporary build described below, 3 runs | 19 cases / 98 assertions each |

`vtextension_test` does not cross-build. The cause predates this task: WAMR's `win_file.c` fails under clang-cl. So the transport cases, the Windows `trySend` included, were checked by compiling `Channel_test.cpp` and `PduStream_test.cpp` into `vtextension_process_test` in the cross tree only. That CMake edit was reverted, and the cross tree rebuilt.

`wineserver -k` was run on my own prefix only.

### Lint

clang-tidy, by hand:

```
clang-tidy -p out/clang-asan --quiet src/contour-extension-host/testing/HostileHost.cpp src/vtextension/wasm/host/InProcessHost.cpp src/vtextension/wasm/remote/HelperIncarnation.cpp src/vtextension/wasm/remote/RemoteBackend.cpp src/vtextension/wasm/remote/RemoteBackend_test.cpp src/vtextension/wasm/remote/process/ExtensionHostSupervisor.cpp src/vtextension/wasm/remote/process/ExtensionHostSupervisor_test.cpp src/vtextension/wasm/remote/process/ProcessHostConnector.cpp src/vtextension/wasm/remote/process/testing/HelperComposition.cpp src/vtextension/wasm/remote/process/testing/LaunchProbe.cpp src/vtextension/wasm/remote/transport/Channel_posix.cpp src/vtextension/wasm/remote/transport/Channel_test.cpp src/vtextension/wasm/remote/transport/PduStream.cpp src/vtextension/wasm/remote/transport/PduStream_test.cpp src/vtextension/wasm/remote/Teardown_test.cpp
```

- First pass: 7 findings, all in tests:
  - `performance-unnecessary-value-param`: the gate future;
  - `readability-use-concise-preprocessor-directives` ×5: now `#ifdef` / `#elifdef`;
  - one `bugprone-implicit-widening-of-multiplication-result`.
- Re-run on the fixed files: 0 findings.
- The `_win32` sources are covered by the clang-cl -Werror build.

`/usr/bin/clang-format --dry-run -Werror` and `editorconfig-checker` on every changed file: clean.

## Deviations, and why

**D1. The backend lets its hosts go, through `IncarnationRoster`. This is the decision the brief did not make.**

The "closing" scenario has W blocked in `load()`. W necessarily holds a `shared_ptr` to the incarnation, and so the handle. No drop of modules or instances can therefore make the incarnation's last reference go, and `kills[Dropped] == 1` / "W returns HostUnavailable" could never happen as D51 is written.

I made dropping the remote backend let go of every host it started:

- `letGo` posts Drop, which gives the right kill reason and grace, and shuts the parent's ends.
- This relaxes the `IWasmBackend` rule that the backend "must outlive every runtime" for the remote backend only. That is memory-safe, because runtimes hold the shared state. It is documented in `RemoteBackend.hpp` and on `~RemoteBackend`.
- In the test, W holds its own copy of the runtime. A raw pointer would have been use-after-free.

**D2. Relay trigger.**

The brief says the relay "stops reading after it forwarded the first Instantiated". With three instances, the second Instantiate would then never be read, and the case would hang.

The relay now stops at the header of the first Load that arrives after an Instantiated. It relays the control channel only; the lanes pass straight through, since only control matters for the scenario.

**D3. Overflow drops are modules, not instances.**

At most 1024 instances (`InterruptSlotCount`) live at once, so 4097 instance drops are impossible. The case loads 4098 modules under one identity, then drops 4097 of them.

**D4. The probe waits for the helper's Hello before it reports.**

Found by the first run: the test killed the probe before the helper had run `bindToParent`. The helper then found the test, its subreaper, as its parent and exited `ParentGone`, and `ignore-eof` slept on. That second part is also why `ignore-eof` now returns any non-zero exit as is.

**D5. Report channel.**

The probe's `--parent` reports on an inherited control channel (`launchHelper`'s), because the launcher gives children a null stdout (D39).

**D6. API additions beyond the brief:**

- `IHostHandle::letGo`, `HelperIncarnation::letGo`, `SupervisedHelper::letGo` (formerly the private `release()`);
- `PduStream::unsentBytes()`;
- `HelperComposition(grace)` and `stopSupervisor()`;
- `RecordingLauncher::exitStatuses()`;
- `ScriptedConnector::lettings()` and `connections()`.

**D7. Renames.** Three 3.3 supervisor cases were renamed to the brief's names rather than duplicated. The one comment that cited an old name was updated.

## Concerns

1. **D1 widens a contract.** A reviewer should confirm that a remote backend going before its runtimes is acceptable. The alternative is to leave the brief's "closing" expectation unmet.
2. **Releases can wait.** Releases queued while the channel said `WouldBlock` go out only with the next release or request. An idle host that read slowly keeps them, so the helper's memory is freed late, until something else is sent. A host that never reads is bounded by the overflow kill.
3. **FreeBSD and macOS paths are compiled only by CI:** `procctl(PROC_REAP_ACQUIRE)`, the `#elifdef __APPLE__` orphan poll, and macOS `MSG_DONTWAIT` with `SO_NOSIGPIPE`.
4. **Windows cancellation:** the cancelled-pipe-write accounting (`trySend` reporting what the reader took before `CancelIoEx`) is verified under wine only, not on real Windows.
5. **Not measured:** performance and coverage, which are 3.7's. No hot-path change: a release encodes one small PDU per try, and a request's drain costs one lock when the queue is empty.

## Fix round 1

### Commit (on f3899d673)
- 078b5be04 `fix(vtextension): send a release queued while a request let go of the channel`

### Items
1. **(Important) Lost wakeup.**
   - The problem: a release queued while a request still held the control mutex after its last send (for example during the StepWatch disarm) found `try_lock` failing. It was then stranded until the next request or release.
   - The fix: new `HelperIncarnation::offerReleases()`. It loops while the incarnation is alive and the queue is not empty: it try-locks, then `trySendReleases()`. It stops when the try-lock fails, because the holder re-checks after it unlocks. It also stops on `ReleaseFlush::Blocked`, meaning the channel is full or failed.
   - `request()` now runs its locked part in a lambda and calls `offerReleases()` after the unlock. `release()` calls `offerReleases()` after queuing.
   - `trySendReleases()` now returns `ReleaseFlush { Blocked, Drained }`.
   - New test `RemoteBackend.releaseQueuedWhileARequestEndsIsSent`:
     - `CountingHandle` gained a one-shot `onNextDisarm` hook, which runs in exactly that window. In it, another thread drops an older instance (bounded at 5 s).
     - The test thread then idles. The case checks that the host read the Release with no further request (`awaitAtLeast(4)`, bounded at 5 s).
     - Mutation: with no `offerReleases()` after the unlock in `request()`, the case fails (`REQUIRE(pdus.size() >= 4)`). The source was restored.
   - Residual: `std::mutex::try_lock` may fail spuriously by the standard. The re-check covers a real holder, not a spurious failure. This does not happen with pthread and SRW mutexes in practice.
2. **`WasmRuntime.hpp`**: `~IWasmBackend` now names the exception, the remote backend (`makeRemoteBackend()`), which tolerates being dropped first.
3. **`release()` doc**: queuing allocates, so running out of memory ends the process (`std::terminate`).
4. **`IncarnationRoster::close()`**: it already takes the whole list, expired entries included, and lists nothing after it is closed. A comment now says so; nothing is left to prune.
5. **`becomeSubreaper()` doc**: the setting holds for the whole test process; later orphaned grandchildren become the binary's zombies, which is harmless.

### Results
- **ASan:**
  - `ctest --preset=clang-asan -R "vtextension|check_wasm_runtime_containment"`: 7/7.
  - `vtextension_remote_test`: 215 cases / 10752 assertions.
  - `vtextension_helper_test`: 145 cases (144 passed, 1 skipped), exit 0.
  - The release/backend-drop cases: 3 runs green.
- **TSan:**
  - `vtextension_remote_test "RemoteBackend.*,HostServer.*,HostProcess.*,InProcessHost.*"`: 3 runs × 76 cases / 7787 assertions.
  - `vtextension_helper_test`: 145 cases.
  - `vtextension_process_test`: 77 cases.
  - 0 ThreadSanitizer warnings.
- **clang-tidy** on the 4 changed .cpp files: 0 findings. clang-format and editorconfig-checker: clean.
