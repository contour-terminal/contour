### Task 3.5: Teardown off the owner thread

**Files:**
- Modify: `wasm/remote/transport/{Channel.hpp,Channel_posix.cpp,Channel_win32.cpp,PduStream.hpp,PduStream.cpp,Channel_test.cpp,PduStream_test.cpp}` (non-blocking send)
- Modify: `wasm/remote/{HelperIncarnation.hpp,HelperIncarnation.cpp,RemoteBackend.cpp,RemoteBackend_test.cpp}` (release outbox, D51)
- Modify: `wasm/remote/process/{ExtensionHostSupervisor.cpp,ExtensionHostSupervisor_test.cpp,testing/LaunchProbe.cpp}` (teardown grace; `--parent` mode)
- Modify: `src/contour-extension-host/testing/{HostileModes.hpp,HostileHost.cpp}` (rows `stop-reading-after-instantiate`, `ignore-eof`)
- Create: `src/vtextension/wasm/remote/Teardown_test.cpp` (vtextension_helper_test; `VTEXTENSION_PROBE_PATH` also defined there)

**Interfaces:**
- Consumes: 3.3, 3.4.
- Produces:
  ```cpp
  namespace vtextension::wasm::remote::transport {
  // Channel gains: sends what fits without blocking. @return bytes sent; 0 if none fit.
  [[nodiscard]] std::expected<std::size_t, ChannelError> Channel::trySend(std::span<uint8_t const> bytes);
  enum class SendOutcome : uint8_t { WouldBlock, Sent };
  // PduStream gains (a partial frame's tail is kept; every later send, blocking or not, sends it first):
  [[nodiscard]] std::expected<SendOutcome, StreamError> PduStream::trySend(proto::Pdu const& pdu);
  }
  namespace vtextension::wasm::remote {
  inline constexpr std::size_t MaxPendingReleases = 4096;
  // HelperIncarnation: post(Pdu) is replaced by
  void release(proto::Release release) noexcept;   // queues; sends now only if the control mutex is free and the
                                                   // channel takes it without blocking; never blocks
  }
  ```

- [ ] **Step 1: Write the transport tests.** `"Channel.trySendNeverBlocks"` (no reader: `trySend` of 4 KiB chunks until it returns 0, inside `std::async`, `wait_for(5s) == ready`); `"PduStream.trySendKeepsAPartialFrame"` (fill until `WouldBlock`; a reader drains; the next `send()` sends the kept tail first; the reader decodes every PDU sent, in order, nothing `Malformed`).
- [ ] **Step 2: Write the outbox tests** (`RemoteBackend_test.cpp`, scripted helper as in 2.3): `"RemoteBackend.releaseWaitsForTheControlChannel"` (a thread's `Instantiate` request is outstanding; the test thread drops an older instance → the drop returns at once (`std::async`, `wait_for(100ms) == ready`); the scripted helper reads `Release{Instance, id}` after it answers the request and before the next request); `"RemoteBackend.releaseOverflowKillsTheHelper"` (the scripted helper stops reading control; `MaxPendingReleases + 1` instance drops → the handle saw `terminate()` once; every drop returned at once).
- [ ] **Step 3: Write the supervisor tests** (FakeProcessLauncher + ManualClock): `"Supervisor.teardownGraceLetsAHelperExit"` (`teardownGrace = 2s`; drop; the fake exits `{ Exited, 0 }` at +1 s → `kills[Dropped] == 0`, `uncleanExits == 0`); `"Supervisor.teardownGraceThenKills"` (no exit by +2 s → `kills[Dropped] == 1`); `"Supervisor.zeroGraceKillsAtOnce"`.
- [ ] **Step 4: Write the Review Focus cases** (`Teardown_test.cpp`, real executables; `RecordingLauncher` records the thread of every `kill()`):
  - `"teardown never blocks the owner thread"`:
    - Section "closing": hostile `stop-reading-after-instantiate`, `teardownGrace = 0`, `timeouts.load = 30s`. Instantiate three instances from InstanceModule on the test thread (the owner). A worker thread W starts `runtime->load(ModuleIdentity{"same"}, <a 16 MiB module>)`, which blocks in its send under the control mutex (the helper no longer reads). The owner then destroys the three instances, the module, the runtime and the backend inside `std::async` → `wait_for(1s) == ready`. `recording.killThreads()` all equal `supervisor->threadId()`. W's load returns `HostUnavailable` within 5 s (the drop killed the helper). After a `sync()` loop: `kills[Dropped] == 1`, `reaped == 1`.
    - Section "quitting": a helper that `ignore-eof`s (and, on POSIX, a second one `SIGSTOP`ped); destroy the supervisor inside `std::async` → `wait_for(1s) == ready`; both processes are gone afterwards (POSIX: `kill(pid, 0) == -1 && errno == ESRCH` once reaped by the destructor or, if it ran out of budget, by this test as their parent; Windows: their handles signalled).
  - `"no helper outlives its parent"`: Linux `prctl(PR_SET_CHILD_SUBREAPER, 1)` / FreeBSD `procctl(PROC_REAP_ACQUIRE)` once for the binary. Launch `extension_host_probe --parent <helper path> [--hostile=ignore-eof]` through the system launcher; read the helper's pid from it; kill the probe (`SIGKILL` / `TerminateProcess`). `DYNAMIC_SECTION` "benign" and "ignore-eof": the helper is gone within 2 s — Linux/FreeBSD `waitpid(helper, WNOHANG)` as its subreaper; Windows a `SYNCHRONIZE` handle opened before the kill signals. macOS: "benign" polls `kill(pid, 0)` until `ESRCH`; "ignore-eof" is `SKIP("macOS has no parent-death signal; documented")`.
  - `"teardown in coverage builds lets the helper exit"` (`teardownGrace = 2s`): drop the backend → the helper exits `{ Exited, 0 }` (recorded), `kills[Dropped] == 0`.
- [ ] **Step 5: Build; expect failures.** `nice -n 19 ionice -c 3 cmake --build --preset clang-asan -j 4 --target vtextension_test vtextension_process_test vtextension_remote_test extension_host_probe contour-extension-host-hostile vtextension_helper_test`.
- [ ] **Step 6: Implement.** `Channel::trySend`: POSIX `send(MSG_DONTWAIT|MSG_NOSIGNAL)`, `EAGAIN` → 0; Windows: an overlapped `WriteFile`, `GetOverlappedResult` without waiting, still pending → `CancelIoEx` and report what was transferred. `PduStream`: `_unsentTail` drained first by `send` (blocking) and `trySend` (`WouldBlock` while it is not drained). `HelperIncarnation::release` (D51): push under `_outboxMutex` (never held across I/O); then `std::unique_lock{ _controlMutex, std::try_to_lock }` and `trySend` the queue front-to-back until `WouldBlock`; over `MaxPendingReleases` queued → `markDead()`. `request()` sends the queue (blocking) before and after its exchange. `~RemoteModule`/`~RemoteInstance` call `release()`. Supervisor teardown grace and `Stop` per 3.3's algorithm with `teardownGrace`. The probe's `--parent`: start a supervisor + `ProcessHostConnector` (helper path and extra arguments from its argv), `connect()`, print the pid (`SupervisedHelper::processId()`), sleep until killed. Hostile rows: `stop-reading-after-instantiate` (the relay stops reading every parent channel after it forwarded the first `Instantiated`), `ignore-eof` (on the parent's EOF it ignores SIGTERM and sleeps forever).
- [ ] **Step 7: Run.** `out/clang-asan/src/vtextension/wasm/remote/vtextension_process_test "Supervisor.*"`, `…/vtextension_test "Channel.*" "PduStream.*"`, `…/vtextension_remote_test "RemoteBackend.*"`, `…/vtextension_helper_test "teardown never blocks the owner thread" "no helper outlives its parent" "teardown in coverage builds lets the helper exit"` → PASS.
- [ ] **Step 8: Commit.** `git commit -s -m "feat(vtextension): let go of an extension host without waiting for it"`

