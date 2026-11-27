### Task 4.7: Gate
- [ ] **Step 1: ASan/LSan.** Build all five test binaries and the three executables; `ctest --preset=clang-asan -R 'vtextension|wasm_runtime_containment'` → all pass. Contract: 42 cases per composition; `uncleanExits == 0`.
- [ ] **Step 2: TSan** (`out/clang-tsan`, the same targets) → zero reports, no suppressions.
- [ ] **Step 3: Measurement** (`out/clang-release`, the two needed targets, three runs each).
  - `Process.measureRender` (steady/uncached) through: the Shared helper, `copy-memory`, the in-process Shared host, and in-process Wasmtime.
  - `Process.measureRoundTrip`, `Process.measureSpawn`, new `Process.measureInstantiate`.
  - Helper and parent VmRSS + Pss (`smaps_rollup`) and VmData after 16 plasma instances.
  - Report against phase 3: steady 18.1–18.6 µs, round trip 3.3 µs, spawn 1.2 ms, RSS 10.8 MiB idle.
  - D83: the steady median must be below 18.1 µs, or stop and report.
  - Record everything in "Phase-4 facts".
- [ ] **Step 4: Windows cross-build + wine** (`linux-to-windows-clangcl-debug`, Embedded debug format, extensions on, bundled and GUI off). Build `vtextension_process_test extension_host_probe contour-extension-host vtextension_helper_test`; five wine runs.
  - **Wine can show:** clean compiles with `-Werror`; `SEC_RESERVE` create/commit; `DuplicateHandle` into a child; `HandleGrant.*`; and the Copy path end to end.
  - **Wine cannot prove:** placeholder mappings (if `canMapGuestMemory()` is false the helper says Copy, recorded); an access violation on uncommitted pages becoming a trap; job accounting of section commits.
  - Record what ran; fix any real break, each in its own commit.
- [ ] **Step 5: FreeBSD/macOS.** Code review against a checklist:
  - XNU `ftruncate` works once;
  - macOS has no `MSG_CMSG_CLOEXEC` (`fcntl` instead);
  - `CMSG` alignment;
  - shm `MAP_FIXED` over an anonymous reservation;
  - the Rosetta bound;
  - FreeBSD `memfd` seals and its parent-only role (WAMR helper in 5.3).
  
  CI results only if the owner allows a push; otherwise record them as outstanding.
- [ ] **Step 6: Coverage** of the SharedObject, HandleGrant, GuestMemoryMapping, MemoryProvision, WasmtimeMemoryCreator, SharedGuestMemory, RemoteBackend and HostServer sources; name the kinds of uncovered lines.
- [ ] **Step 7: Phase review** (`/code-review` of `4ec0d13f6..HEAD`); address every finding.
- [ ] **Step 8: Phase summary:** performance (Step 3), risk (D69, D74, D75, D76, D81), coverage, and what wine and CI did and did not prove.

### Phase-4 facts
*(Filled in by 4.4 and 4.7.)*

From 4.4 (`c2bd2249e`):
- **Wasmtime 48.0.10 memory creator, measured** (GrowModule, min 1 / max 16 pages, cap 4 MiB): `new_memory` gets `minimum = 65536` (initial bytes), `maximum = 1048576` (the module's declared max in bytes, not the cap), `reserved_size_in_bytes = 4194304` (= `memory_reservation` = `memoryObjectBytes(cap)`), `guard_size_in_bytes = 0` under Software and `33554432` under Signals. `grow_memory(env, newSize)` gets the new **total** byte length (131072 for `memory.grow 1` from 1 page). An error from `grow_memory` makes `memory.grow` return -1 (no trap) with the store limiter the backend installs. `get_memory` returns base, size (= current bytes) and capacity (= object bytes).
- `IHostedEngine::memoryGuardBytes()` is new: the guard the host maps after each object (Wasmtime Provided: 32 MiB Signals / 0 Software; Copy engines 0). The **server** maps the grant (`GuestMemoryMapping::map(handle, objectBytes, initialBytes, memoryGuardBytes())`) before instantiating, adopts the handle and closes it; `MemoryProvision` is `{GuestMemoryMapping memory; GrowRequest grow}` (not `{OwnedHandle, sizes, grow}`).
- Server grant rules (Shared): `objectBytes == memoryObjectBytes(CreateRuntime.memoryBytes)`, `initialBytes <= objectBytes`, `initialBytes % 64 KiB == 0`, claim succeeds, map succeeds; else ProtocolViolation. Copy: `memory == MemoryGrant{}` and nothing attached. Any non-Instantiate control PDU with an attached descriptor is a violation.
- The creator requires `minimum == initialBytes`, `reserved == objectBytes`, `guard <= mapping guard`; a module whose memory differs fails with `Instantiated{InvalidComponent}` (not a violation). The **parent must send `initialBytes` = the module's initial memory exactly** (D72).
- Growth: `Grow{instanceId, newTotalBytes}` then exactly one `Grown`; on the lane of the running call, or on **control before `Instantiated`** for a start function (R29). Anything else answering it is a violation. Helper opens (POSIX `mprotect`, Windows `VirtualAlloc(MEM_COMMIT)` through its own view) only after `Granted`.
- wine 11.20 supports `VirtualAlloc2`/`MapViewOfFile3` placeholders: `canMapGuestMemory()` is **true** under wine; the shared-memory server tests pass there.
- `makeHostedEngine(proto::MemoryMode = Copy)` (neutral type, not `wasmtime::MemorySharing`, because testing/ may not name the backend). `HelperEngine` is still Private/Copy.

From 4.4 fix round 1 (`3780e3062..891522849`):
- **Signal traps on provided memory (Wasmtime 48, release tree, strace):** Wasmtime bounds an access by the **reservation** (= object, 4 MiB), not the current size. A store past the open bytes but inside the object passes the check and faults `SEGV_ACCERR` in the closed pages → handled as a trap (so the helper's `PROT_NONE` tail / Windows uncommitted pages are **the** bounds enforcement there, not a belt). A store past the object fails the explicit check and is redirected to address 0 (Spectre hardening) → `SEGV_MAPERR` at NULL → trap. The 32 MiB guard is never touched with a 4 MiB reservation; no abort over `wasm_accessible()` was seen. Capacity reported to Wasmtime stays `objectBytes`.
- Catch2 restores its own signal handlers after each test case, over Wasmtime's once-per-process ones: only **one** test case per binary may bring up a Signals engine (`WasmtimeEngine.signalTrapsOnProvidedMemory`).
- `GrowRequest` is now `GrowStatus(GuestMemoryMapping& memory, uint64_t bytes)`: it asks the parent and opens on grant. Granted-but-unopenable → `ServerExit::MemoryUnusable` → `HostProcessExit::MemoryUnusable`, exit code **76**, connection ends once the guest returns (no `CallResult`/`Instantiated` is sent). `HostServerConfig::openGuestMemory` (default `&GuestMemoryMapping::open`) is the test seam.
- `MemoryProvision` lives in its own target `vtextension_memory_provision` (links `vtextension_remote_wire`); `vtextension_wasmtime` links it privately, not `vtextension_host`.
- POSIX reservation has no `MAP_NORESERVE` (FreeBSD lacks it).

From 4.5 (`9efa54729..41aeac18e`):
- **Linux glues bytes queued before an `SCM_RIGHTS` message into the same `recvmsg`** (it stops *after* the
  descriptor-carrying skb, not before). `PduStream` attributes a descriptor to the last frame starting in its
  buffer at receipt (Linux) or the first frame starting in the chunk (BSDs, by reading); `takeAttachedHandle()`
  hands it out only once that frame was received. Any relay or reader of granted descriptors must do the same.
- **Wasmtime 48 installs its signal handlers lazily, by the first instantiation**, not at engine creation. Catch2
  (a compiled library here: `CATCH_CONFIG_NO_POSIX_SIGNALS` cannot apply) restores its own after each case;
  `WasmtimeEngine_test`'s `EngineSignalHandlers` puts Wasmtime's back per Signals case. Two Signals cases now.
- R30 lives in `HostConnection::objectBytesCap` (process connector: `caps.dataBytes`; in-process default 1 GiB;
  0 = none), leased per instance (`HelperIncarnation::leaseObjectBytes`). With the 4 MiB contract cap only 256
  instances fit: tests holding `InterruptSlotCount` instances use a one-page runtime.
- `HelperIncarnation::request(pdu, step, grant, GrowAnswer)` answers control-channel Grows under the control
  mutex; the two-argument `request` treats a Grow as a wrong reply.
- The helper's Hello says Shared iff `canMapGuestMemory()`; `makeCopyingHelperEngine()` exists for the hostile
  `copy-memory` row (the D83 copy-versus-shared measurement). `makeHostedEngine()` defaults to Shared; scripted
  HostServer/HostProcess tests ask for Copy.
- The helper sets `RLIMIT_NPROC = 0` (release): a hostile-host thread must start before `serve()`. The hostile
  setup-failure code is 79 (76 is MemoryUnusable). Copy-path hostile rows run `HostMemory::Copied`.
- Release tree: the real helper with Signals traps and Provided memory passes `vtextension_helper_test
  "~[.measure]"` whole (157 cases). Under `scribble-memory` 199/200 renders succeed in both trees.

From 4.6 (`e4ffcfe5f..3a49108ff`):
- `abi::IGuestMemory` is `read(Address, span<byte>)` (one copy; the only way lifting reads), `writable(Address,
  size)` (lowering and range proofs only), `allocate`, `chargeLift`. `detail::readInteger<U>`,
  `detail::readBytes<Container>(memory, list, elementSize)` (byte-sized containers; caller charges first),
  `detail::checkElements` (range proof, reads nothing); `loadPointerAndLength` is one 8-byte read. Any new
  decoder reads through these; `"GuestReader.fetchesEachByteOnce"` (decoy `writable()`, per-byte read counts)
  over every lifted type and each control request catches one that does not.
- `RemoteRuntime::load` reads the module's initial memory before any host is asked (unreadable bytes:
  `InvalidComponent`, no Load sent, no slot made live); `RemoteModule::instantiate` refuses an over-cap initial
  memory before `_slot->live()`. A test "module" of zero bytes is now refused up front: use a real module
  (Teardown_test pads one with a custom section).
- `extension_host_probe --mutate <helper args> --seed=N`: claims one Instantiate's grant, commits the whole
  object, flips random bytes (`mt19937{N}`) on a thread until control EOF; reports `mutating`/`end`, then
  `mutations=N`/`end`, exit 0. The seed is an argument because clang-tidy refuses a constant-seeded engine.
- The `scribble-memory` row is unchanged by single-fetch (199/200 in ASan and release): it scribbles the whole
  memory of the latest instance, so the one failure is the guest's own, not a host decode. The single-fetch
  proof is the deterministic GuestReader test; the hostile decode test (`HostileMemory_test.cpp`) did not catch
  a validate-in-place-then-copy mutation in 3 runs (the window is too narrow), it proves robustness only.

## Review Focus addendum (phase 4)
6. **A helper mutates memory while Contour decodes.** No decoder may read a byte twice. Pinned by `"GuestReader.fetchesEachByteOnce"` and `"hostile memory does not break decoding"`; review rule: no decoder reads through `writable()`.
7. **Contour touching memory it never granted.** A helper's growth past the cap, shrink or wrong instance; reads past the acknowledged size; Windows uncommitted pages. Pinned by `"SharedGuestMemory.growthIsChecked"`, `"RemoteBackend.badGrowKillsTheHost"` and the grow hostility rows.
8. **Descriptor leaks either way.** Contour must install no descriptor a helper sends, and hold none per instance. Pinned by `"Channel.parentInstallsNoDescriptorFromTheHelper"`, `descriptor-up` and `"RemoteBackend.instancesHoldNoDescriptor"`.
9. **Growth on unusual paths:** during nested `cabi_realloc` lowering, while interrupted, or in a start function. Pinned by `"RemoteBackend.growDuringLoweringIsSeen"`, `"RemoteBackend.growWhileInterruptedIsRefused"`, `"HostServer.growOutsideALaneIsRefused"` and `grownMemoryIsVisible`.
10. **Unproven platforms.** Rosetta/XNU rounding, Windows placeholders and macOS ancillary handling are verified by reading and CI only. D81 also leaves guest memory outside RLIMIT_DATA.

## Task order
4.1 → 4.2 → 4.3 → 4.4 → 4.5 → 4.6 → 4.7 (4.6 depends only on 4.2 and may run beside 4.3–4.5).

## Phase-4 decisions

Proposed for a controller ruling. Each names the choice and, after the dash, why.

- D62. Seven tasks: 4.1 carried minors → 4.2 shared objects → 4.3 handle passing + wire v3 → 4.4 helper side → 4.5 parent side (Shared switched on) → 4.6 single-fetch decoding → 4.7 gate. — Growth spans both processes, and each side tests alone against a scripted peer. The minors touch files 4.3 edits. 4.6 needs only 4.2.
- D63. One switch, `CONTOUR_BUILD_INSTRUMENTATION` (0 none / 1 coverage / 2 sanitizers). It is a PUBLIC definition on `vtextension_remote_wire`, read only by `wasm/BuildInstrumentation.hpp` (`enum class BuildInstrumentation : uint8_t { None, Coverage, Sanitizers }`, `ThisBuildInstrumentation`). It replaces `CONTOUR_SANITIZED_BUILD`, `CONTOUR_INSTRUMENTED_BUILD` and `CONTOUR_HELPER_SOFTWARE_TRAPS`; caps, helper stderr and trap mode become constexpr reads. — Three macros with three conditions become one value.
- D64. M2: `LinuxProcessLauncher(PidfdWaits)`; `makeSystemProcessLauncher()` probes once and injects; `makeLinuxProcessLauncher(PidfdWaits)` goes in a Linux-only `process/Process_linux.hpp`. — DI without widening the cross-platform API.
- D65. M3: one `launchErrors()` table in `ProcessLauncher.hpp`, rows `{error, text, refusal}`. `launchErrorText`, the connector's refusal mapping and `refusalLine()` read it; the launch log lines become `"The extension host cannot be started: {text}. Extensions do not run."`. — Each failure is phrased once.
- D66. `hardenProcess()` returns `std::expected<void, HardeningStep>` over per-OS rows `{step, text, apply}` (`CoreDumps`, `Dumpable`, `Tracing`, `FaultReports`). Every row runs, and the first failure is returned. `beforeThreads` takes that type. The helper tells `hardening failed: <text>` once, and Hello is still `Failed`. — The log names the step, and hardening stays complete.
- D67. One creation module, `transport/SharedObject{.hpp,_posix,_win32}`, for the control page and guest memory (as D46 foresaw). `fitsSharedObject(object, expected)` accepts sizes from `expected` up to `roundUp(expected, MaxPageBytes = 16 KiB)`, whatever the reader's own page size. — Fixes M1: an x86_64 helper under Rosetta sees a 4 KiB page while the arm64 parent's object was rounded to 16 KiB.
- D68. One object per instance, always, even for modules without memory.
  - Size: `proto::memoryObjectBytes(cap) = max(roundUp(cap, 64 KiB), 64 KiB)`.
  - Linux/FreeBSD ≥ 13: memfd sealed `SHRINK|GROW`, then `SEAL` once mapped. GROW goes beyond the spec's `SHRINK|SEAL`; it is strictly stronger.
  - macOS: `/contour-eh-<pid>-<serial>`, `O_EXCL`, unlinked at once, `ftruncate` once.
  - Windows: a `SEC_RESERVE` section.
  - The parent maps the whole object RW once (it never moves) and closes its handle once the `Instantiate` carrying it is sent. — No descriptor per instance (RLIMIT_NOFILE with many panes).
- D69. Handle passing rides the **control channel with `Instantiate`** (already there per D9/D11).
  - POSIX: `SCM_RIGHTS` on the frame's first byte. Only the helper's control stream (Control, arriving ToHelper) uses `recvmsg` (`MSG_CMSG_CLOEXEC`; macOS `fcntl`) and holds at most one unclaimed descriptor. A second one, `MSG_CTRUNC` or a non-`SCM_RIGHTS` cmsg → `Malformed`.
  - Windows: the parent `DuplicateHandle`s the section (`FILE_MAP_READ|FILE_MAP_WRITE`) into the helper. It uses a `PROCESS_DUP_HANDLE` duplicate that the launcher hands out (`IProcessHandle::handleTarget()` → `SupervisedHelper` → `IHostHandle::handleTarget()`). The value travels in `Instantiate.memory.handle`, which must be 0 on POSIX.
  - **Lost:** a failed send kills the incarnation, and the handle dies with the helper.
  - **Forged:** the helper refuses what it cannot map as an object of `objectBytes` with write access, as a protocol violation. A bogus Windows value is released, not closed.
- D70. Nothing flows up. Parent reads stay plain `recv()`, so the kernel discards descriptors a helper attaches; on Windows the parent never reads a handle value. — Pinned by a test and a hostile row.
- D71. Wire v3:
  - `Instantiate.memory = MemoryGrant{u64 handle, objectBytes, initialBytes}`; `objectBytes == 0` means a copy-mode host. Row cap 32 → 64.
  - `Grow{u32 instanceId, u64 bytes}`: Lane, ToParent, cap 16.
  - `Grown{GrowStatus}`: Lane, ToHelper, cap 8; `GrowStatus { Refused, Granted }`. Kinds 16 and 17.
  - The `Memory*` PDUs stay for Copy; `DecodeError` is unchanged.
- D72. The initial size comes from the parent's own parse (`binary::largestInitialMemoryPages(_bytes)`). Above the cap → `OutOfMemory` before anything is sent. The helper's creator requires Wasmtime's `minimum == initialBytes` and `reserved == objectBytes`.
- D73. Wasmtime config in Provided mode:
  - `memory_reservation = objectBytes`.
  - `memory_guard_size` = 32 MiB under Signals, 0 under Software.
  - `reservation_for_growth = 0`, `may_move = false`, `init_cow = false`.
  - `wasmtime_config_host_memory_creator_set`.
  - `WasmtimeConfig` gains `MemorySharing { Private, Provided }`.
  - The helper mapping (`transport/GuestMemoryMapping`, std-only so it runs under wine) reserves object + guard inaccessible, maps the object `MAP_SHARED|MAP_FIXED` at the base and opens only `[0, initial)`.
- D74. Windows maps through placeholders (`VirtualAlloc2` / `MapViewOfFile3` / `UnmapViewOfFile2`), resolved from kernelbase by `GetProcAddress`, plus a 64 KiB probe (`canMapGuestMemory()`). If unavailable, the helper's Hello says `Copy` and the copy path serves. — Stated in the handshake, equally safe.
- D75. **Deviation in wording:** the provision reaches the engine-wide creator through an engine-neutral thread-local (`host::MemoryProvision`, RAII `ProvisionScope`). HostServer's **control thread** sets it, not "the executor" (spec §5.4), because instantiation runs on control (D11). The creator takes it once; none, or a second memory → `InvalidComponent`.
- D76. Growth.
  - Helper: `grow_memory(n)` sends `Grow` on `CurrentLane` and reads exactly one PDU, which must be `Grown`. Granted → `open(n)`; Refused → `memory.grow` returns −1.
  - **Outside a lane call (a start function) growth is refused**, as host calls are there (D11). This differs from in-process engines.
  - Parent, in every call loop: a violation unless the id is the innermost frame's, `n % 64 KiB == 0` and `acknowledged < n ≤ min(cap, objectBytes)`.
  - Parent answers: interrupted → Refused; Windows commit failure → Refused; otherwise commit, store `n` (release) and answer Granted.
- D77. `memory()` is bounded by the parent's acknowledged size (acquire), never by anything the helper reports. A module that exports no memory → `OutOfBounds`. Views stay valid across growth.
- D78. The mode follows each incarnation's Hello. Both modes are accepted: `HostRefusal::MemoryMode` is removed, and `RemoteInstance` holds `variant<CopyPathMemory, SharedGuestMemory>`.
- D79. Single fetch: `abi::IGuestMemory::read(Address, span<byte> into)` (one memcpy) becomes the only way decoders read guest bytes. `view()` is renamed `writable()` (lowering and range proofs only). — Enforced by `"GuestReader.fetchesEachByteOnce"`, whose `writable()` hands decoders a decoy buffer.
- D80. Hostile memory is tested two ways, with no new executable: `extension_host_probe --mutate` for the abi-level case, and the hostile relay's `scribble-memory` row end to end.
- D81. **Residual risk, needs a ruling.** Guest memory leaves D38's RLIMIT_DATA (Linux does not count `MAP_SHARED`). A compromised helper can dirty whole objects of all live instances (≤ 1024 × 32 MiB), keep released ones mapped until it dies, and on Windows commit section pages through its own view. Proposed: accept and document. The alternative is a per-incarnation byte budget.
- D82. Compositions: remote_test's in-process host runs Shared, plus a Copy composition (`makeHostedEngine(Private)`); wasmtime_test stays Private; the real helper runs Shared. The contract gains `grownMemoryIsVisible` (42 cases).
- D83. Gate: the phase closes only if the real helper's steady plasma median is below 18.1 µs (phase 3's best). Overhead is reported as (helper − in-process Wasmtime) / helper, against the copy path in the same build via the hostile row `copy-memory`.
- D84. Phase 5 inputs:
  - seccomp: `recvmsg` on control, `mmap(MAP_SHARED|MAP_FIXED)` and `mprotect` of received descriptors, `close`.
  - Seatbelt: map received shm (`ipc-posix-shm-*` by the `/contour-eh-` prefix).
  - AppContainer: map a duplicated-in section.
  - Landlock is unaffected.

Builds: `flock -o /tmp/contour-extension-host.build.lock nice -n 19 ionice -c 3 cmake --build --preset clang-asan -j 4 --target <t>`. Tests start with `unset $(env | grep -o '^CONTOUR_[A-Z_]*')`. Binaries are under `out/clang-asan/src/vtextension/` (`vtextension_test`), `.../wasm/remote/` (process, remote and helper tests) and `.../wasm/wasmtime/`. Before each commit: `ctest --preset=clang-asan -R vtextension`, clang-format, editorconfig-checker. After each task: `/simplify`.

Rulings on D62–D84 (R28–R31), applied by the controller:
- R28. D62–D84 are accepted, except as changed below.
- R29 (changes D76). Growth during instantiation, for example a start function calling `memory.grow`, is allowed and travels on the **control channel**. While the parent waits for `Instantiated`, it answers a `Grow` there with `Grown`, under the same checks. `Grow`/`Grown` gain a Control row in each direction they need. Why: memory growth is not a host call, a C or Rust runtime commonly grows memory in its start function, and refusing it would make out-of-process behave differently from in-process. Cost if wrong: one more channel row and a test (`"HostServer.growDuringInstantiateGoesThroughTheParent"`, which replaces `growOutsideALaneIsRefused`).
- R30 (changes D81). The parent caps the sum of `objectBytes` across one incarnation's live instances at `ResourceCaps::dataBytes` (1 GiB by default). An `Instantiate` that would exceed it fails with `OutOfMemory` before anything is created. This bounds the memory a compromised helper can dirty. What still remains is documented: released objects can stay mapped until the helper dies, and on Windows the helper can commit pages through its own view (the job memory limit counts those). Test: `"RemoteBackend.liveObjectsStayWithinTheDataCap"`.
- R31 (changes D83). The gate passes when the real helper's steady median is at or below phase 3's range (≤ 18.6 µs) and the copy-versus-shared delta is reported. It stops only on a regression. Why: a steady render moves few guest bytes, so "strictly faster than 18.1 µs" would gate on noise.

---

## Phase 5 — Sandboxes, and WAMR in the helper


### Item 0 — review minors carried from the previous task (first, own commit(s))
1. `HostileMemory_test.cpp:68-95` copies `Instance::GuestMemory::read` into `ScribbledMemory` instead of running the real one. Add a small testing seam so the hostile test builds the production `GuestMemory` over a `SharedGuestMemory` (or equivalent) and drop the copy.
2. `CanonicalAbi_test.cpp:673` `GuestReader.fetchesEachByteOnce` covers only `Codec<T>::load`; add a flat-lifting pass (`lowerFlat` then `liftFlat` with the decoy active) for the types production lifts flat (control requests, run-command name/args).
3. `LaunchProbe.cpp:25` and `:449-451` claim a seeded run repeats; `std::uniform_int_distribution` differs across standard libraries. Either derive offsets from raw `mt19937` output or state "same toolchain only".
4. `RemoteBackend.cpp:693-696` comment: "…which kills the incarnation" overstates; say "a request that fails before the host claims it means the host is gone".

### Controller notes (binding; they override the step text where they differ)
- **R31 overrides D83 / Step 3's "below 18.1 µs":** pass when the real helper's steady median ≤ 18.6 µs; report the copy-versus-shared delta; stop and report only on a regression.
- **Step 5:** the owner has NOT allowed a push. Do the reading checklist and record CI items as outstanding. Also add to the outstanding list: `PduStream.descriptorGoesWithItsFrame` and `instantiatesConcurrently` on macOS/FreeBSD (the BSD `attachedFrameStart` branch is verified by reading only), and the real-Windows cross-view commit test.
- **Step 4** must also compile the 4.6 additions (probe `--mutate`, `HostileMemory_test`) for Windows.
- **Step 7 is done by the controller, not you:** do not run `/code-review` (it spawns agents). Stop after Step 6 and the Step 8 draft; the controller runs the phase review and sends you the findings to fix.
- **Step 8:** write the phase summary as a section `## Phase 4 summary` in your report file (performance table vs phase 3, risk for D69/D74/D75/D76/D81 incl. R30, coverage numbers per source, what wine proved / did not, CI outstanding).
- Long task: put a timestamp (`date '+%Y-%m-%d %H:%M:%S'`) at the start and end of each step in the report so the controller can log time per step.
- TSan tree: `out/clang-tsan` may need configuring (`PACE_SCHEDULER=127.0.0.1:6674 cmake --preset clang-tsan` plus the extension options the asan tree uses); release tree `out/clang-release` exists. Coverage: `clang-coverage` preset, same option set. Build only the targets needed, one tree at a time, flock/nice/ionice/-j4. Delete nothing in other trees.
