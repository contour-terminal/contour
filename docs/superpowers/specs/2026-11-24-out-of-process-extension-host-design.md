# Out-of-process extension host with a compiling WebAssembly engine — design

Date: 2026-11-24 · Revision 3 (after two independent reviews) · Status: draft for approval ·
Owner: Christian Parpart

Markers: **[decided]** = decided by Christian in conversation; **[call]** = the author's call, open to
correction; **[confirm]** = the author's call on a question that is really the owner's (§10).

## 1. Intent

Extensions run on WAMR's fast interpreter today, in-process, with every JIT and AOT mode off. That was
deliberate: no executable memory is ever mapped, and no process-wide signal handler is installed. It is
now too slow. The agent orchestrator (`feature/agent-orchestrator`, Yaraslau and Christian) already had
to shrink `gh` replies at the source (`--jq`) to make a 63 ms parse fit. Its `update(procs)` callback
still takes ~35 ms against a 50 ms (status line) / 200 ms (daemon world) deadline, and more complex
guest work is coming.

So: **run guest code as native code, and pay for that where it cannot hurt.** Each extension runs in
its own sandboxed helper process, **`contour-extension-host`** **[decided]**. That process may map
executable memory and install fault handlers. A crash, a hang or an exploited engine bug takes down
that extension and nothing else. This is Chrome's renderer model.

## 2. Goals, non-goals, anti-goals

**Goals**

- **G1. Speed.** Guest code is compiled (Wasmtime/Cranelift **[decided]**). Target: the orchestrator's
  `update(procs)` on `prs-100.json` runs ≥ 5× faster end to end (IPC included) than the 35 ms median
  measured on the interpreter (§9).
- **G2. One helper per extension** **[decided]**, per hosting process (the GUI and `contour daemon` each run
  their own). All of an extension's pane instances live in its helper.
- **G3. Fault isolation.** A crash, abort, OOM kill, hang or protocol violation of a helper never takes
  down the GUI or the daemon. It never stalls their threads beyond the existing per-call deadline plus a
  bounded grace period, and never stalls the GUI thread at all. It counts as one fault of that extension.
- **G4. Containment** **[decided, OS sandbox included]**. A compromised helper (an engine bug exploited by
  a malicious `.wasm`) holds nothing beyond its channels and its guest memories: no file system, no
  network, no new processes, no ptrace, no environment, no inherited descriptors. It can crash or
  stall only itself. It can feed the parent garbage, which the parent treats as hostile, and guest
  memory as concurrently mutable.
- **G5. Every extension benefits:** bundled and user-supplied `.wasm`, status-line and daemon world.
- **G6. Small, named changes above the backend seam** (§5.9). The worlds, the worker pool,
  `DeadlineGuard` and the WIT packages keep their behaviour.

**Non-goals.** Changing the extension API, WIT worlds, manifests or grants. Guest threads, WASI p2, and
the component model in the engine (the Canonical ABI stays hand-written on the host). Making the in-process
interpreter faster.

**Anti-goals** (failure even if it works)

- User-facing names, config keys or docs that say "wasm", "WAMR" or "Wasmtime". The helper is
  `contour-extension-host`. Only internal code names the engine.
- Any JIT, executable mapping or signal handler in the GUI or daemon process.
- Idle cost. No helper is started without a configured extension, and an idle helper never wakes up (no
  ticker).
- Visible status-line latency or terminal-throughput regressions (the `docs/extensions.md` figures hold).
- **A sandbox that degrades silently.** A helper that cannot lock down refuses to run guest code. The only
  unsandboxed path is a build option, and it is reported (§5.6).

## 3. Constraints (verified against the tree)

- **Seam:** `src/vtextension/wasm/WasmRuntime.hpp`, six interfaces (`IHostContext`, `IWasmInstance`,
  `IWasmModule`, `IWasmRuntime`, `IThreadGuard`, `IWasmBackend`). Everything is synchronous. `call()` runs on
  the calling worker thread, and every host import runs synchronously on it as
  `IHostContext::callHost(index, flat i32 args)`. The host reads and writes guest memory through
  `IWasmInstance::memory()`. Arguments are also lowered **outside** host calls, before `call()`
  (`Instance.cpp:421, 439`), and every lowered string/list is a nested `cabi_realloc` guest call
  (`Instance.cpp:99-109`). `interrupt()` is sticky and is called from the **single process-wide watchdog
  thread** while it holds the mutex that every lane takes to arm and disarm (`Engine.cpp:103-115`).
  `makeDefaultBackend()` takes no configuration. The contract suite
  (`wasm/testing/BackendContract.hpp`) has `installsNoSignalHandlers`, `twoBackendsAgree` and
  `generationFollowsBringUp`, and `makeDefaultBackend()`/`preparedGeneration()` are link-time singletons.
- **Loading:** one `IWasmRuntime` per `Engine`. The status line loads each module **once per pane** and
  again on every `SessionExtensions::rebuild` (`ExtensionHost.cpp:98`). The daemon world loads once,
  synchronously on its owner thread (branch: `DaemonExtensionRuntime.hpp:173`). Retired modules are released
  on the **GUI/owner thread** (`ExtensionHost.hpp:44-55`, `ExtensionHost.cpp:44-48`).
  `CompiledExtension::load` maps every load error to `InvalidComponent` (`CompiledExtension.cpp:129-131`).
- **Identity:** the grant store already keys an extension as `<config key>@<canonical module path>`
  (`ExtensionGrantStore.hpp:54`), shared across profiles.
- **Concurrency:** an extension's instances in different panes run on up to four worker lanes at once,
  while calls into one instance never overlap. Fault accounting is per `Runner`, so per pane (`Runner.cpp`).
- **Limits:** 32 MiB memory, 128 KiB stack, 50 ms call deadline (status line), 200 ms call and 500 ms load
  deadline (daemon world). The load deadline guards only `_initialize` and the guest's `load` export, not
  compilation. Durable host calls **and `run-command`** (≤ 5 s) pause the deadline. Modules are ≤ 16 MiB,
  host-call payloads ≤ 64 KiB. Per orchestrator callback there are about **8** durable store writes
  (160 per 20-call run), capped at 64 (`DurableBudget.maxCalls`).
- **Spawning:** `vtpty`'s spawn machinery is POSIX-only, and its launch modes do not give a waitable plain
  child. `PR_SET_PDEATHSIG` binds to the spawning *thread* (`KeeperProcess.hpp`). Children whose status
  another reaper took already occur in the tree (`StdioChild_posix.cpp:154`, `Process_unix.cpp:541`), so
  PID-based kill or wait is unsafe. Parent descriptors can leak into other children (`QProcess`; macOS has
  no `SOCK_CLOEXEC`).
- **Packaging:** Flathub and Fedora Koji build **offline** (`CONTOUR_USE_CPM=OFF`). The RPM already ships
  without the bundled guests, because their crates are not vendored (`contour.spec:113`). The macOS
  bundle signs every nested binary with the hardened runtime, and entitlements go on the outer bundle only
  (`scripts/macos-bundle.py:503-507`). Windows ships a per-user MSI and a zip
  (`build.yml:1600-1602`). Packages: DEB, both RPMs (`contour`, `contour-daemon`), Flatpak, AppImage,
  macOS DMG, Windows MSI/zip, FreeBSD.
- **CI:** Linux x86_64 and arm64, macOS 15, Windows, FreeBSD. There is no Flatpak job in this repository.
- **Master vs branch:** the daemon world, `DaemonGuest_test`, `prs-100.json`, `contour extension list`,
  `vtpty/ProcessRunner` and a world-taking `Engine::create` exist on `feature/agent-orchestrator` only.
- **Project rules** (AGENT.md): C++23, `std::expected`, DI for every OS resource and clock, complete
  constructors, data-driven tables, `enum class` over `bool`, zero warnings with no suppression,
  Catch2 tests, and `vendor/core-cpp` left untouched.

## 4. Approaches considered

| | Approach | Trade-off |
|-|----------|-----------|
| A | WAMR AOT (`wamrc`) at build time for bundled guests | Smallest change. User `.wasm` stays slow, `.aot` is tied to an architecture and a WAMR version, and LLVM is needed at build time. |
| B | WAMR LLVM JIT in the helper | Compiles everything, but links LLVM (tens of MB), compiles slowly on first use, and Fast JIT is x86-64-only with modest gains. |
| **C** | **Wasmtime in a sandboxed helper, a new backend beside `wamr/`** | Compiles every module and is the most security-reviewed of the three. Its epoch interruption fits our deadline model. Costs: a new dependency (~30 MB library), offline-packaging work, a weaker macOS entitlement (§5.6), and FreeBSD is Tier 3 for Wasmtime. |

**Picked: C** **[decided]**. WAMR stays as the engine for platforms or packages without Wasmtime. It
runs *inside the same sandboxed helper*, so isolation is the same everywhere and only the speed differs.

## 5. The design

### 5.1 Shape

```
 GUI / daemon process                                    contour-extension-host (one per extension)
 ────────────────────                                    ──────────────────────────────────────────
 ExtensionHost ─ ExtensionRegistry (identity, breaker)
   │ IWasmBackend (+ ModuleIdentity on load)
 RemoteBackend ── ExtensionHostSupervisor (one thread)
   │   spawn / reap / kill / timers, via process handles
   ├── lane channel 0..3 ─────────────────────────────►  executor 0..3 ─┐
   ├── control channel  ──────────────────────────────►  control thread ├─ IHostedEngine
   └── guest memories (parent-created, mapped RW) ◄───►  linear memories ┘   ├ WasmtimeEngine
                                                                              └ WamrEngine
```

- **`RemoteBackend`** (parent) implements the six interfaces by forwarding. It links no engine.
  `makeDefaultBackend(BackendConfig)` gains a config: the helper path, timeouts and the launcher (DI)
  **[call]**.
- **`contour-extension-host`** is a thin `main` over `HostServer`, which drives an **`IHostedEngine`**. This
  is a helper-only interface: the existing backend operations plus "map this provided memory object as
  linear memory" and the trap mode. It has two implementations, `WasmtimeEngine`
  (`src/vtextension/wasm/wasmtime/`) and `WamrEngine` (adapting the existing backend). Both implement
  shared memory, WAMR through its own memory allocator hooks **[call]**; if WAMR cannot place linear
  memory into a provided object, `WamrEngine` falls back to the copy path (§5.4). The handshake reports the
  memory mode, so the parent knows whether to create memory objects.
- **Build matrix:** `CONTOUR_EXTENSION_HOST=out-of-process` (default) | `in-process`. In-process is the old
  in-process WAMR interpreter, for platforms without a sandbox implementation and for debugging.
  `CONTOUR_WASM_RUNTIME=wasmtime|wamr` picks the helper's engine. `in-process` + `wasmtime` is a configure
  error **[call]**.
- **Contract changes, stated once here:** `IWasmRuntime::load` gains `ModuleIdentity`. `CallFailure` gains
  `HostCrashed`. `ExtensionError` gains `HostUnavailable`, which `CompiledExtension::load` passes through
  instead of mapping to `InvalidComponent` **[call]**. "No signal handlers" binds the parent process
  only. `RemoteBackend` meets `enterThread` and `runtimeGeneration` trivially (generation = bring-ups of
  its first runtime; a trivial guard). `twoBackendsAgree` is met by one shared per-kind state object that
  the backends receive by injection, not through a global **[call]**.

### 5.2 Identity, registry and breaker (above the seam)

- **Identity** **[call]** = the existing grant `hostId` (`<config key>@<canonical module path>`) plus the
  world, plus a hash of the extension's effective configuration (grants, `run_command`, config map). Two
  profiles with the same entry and the same effective configuration share one helper. If anything
  differs, they get two helpers.
- **`ExtensionRegistry`** (new, in `vtextension`, one per hosting process) maps identity + module hash to
  one `CompiledExtension`. Panes take a reference and instantiate from it. A rebuild with unchanged
  identity and bytes reuses the entry and its helper. The last reference dropping releases the entry
  (§5.3, teardown).
- **Breaker** in the registry entry: it uses `ExtensionLimits.faultsBeforeDisable`/`faultWindow` and an
  **injected clock**. It counts helper deaths (not guest traps, which `Runner` keeps counting per pane). One
  death counts once, however many panes it hit. While open, `instantiate()` fails fast with
  `HostUnavailable` and spawns nothing. **Every config reload resets every breaker**, whether its entry
  was reused or replaced, which preserves today's "disabled until reload" behaviour.
- **Incarnations:** every instance is bound to the helper incarnation that created it. After a death, the
  old instances fail every call with `HostCrashed` at once, and the next `instantiate()` (lazily) spawns
  a new incarnation. The entry keeps the module bytes (≤ 16 MiB) or its cached artifact for that.

### 5.3 Process lifecycle — `ExtensionHostSupervisor`

One supervisor thread per hosting process **[call]**. It owns every spawn, reap, kill and timer. It never
blocks on a helper's channel, and nothing else ever kills or waits.

- **Launch** through an `IProcessLauncher` (DI), implemented fresh on master (`vtpty/ProcessRunner` exists
  only on the branch). It produces a waitable **direct child** referenced by a **process handle**: `pidfd`
  on Linux, `pdfork`/process descriptor on FreeBSD, kqueue `EVFILT_PROC` on macOS, `HANDLE` on Windows.
  The child gets a clean environment, cwd at the filesystem root, and exactly its channel descriptors
  (`posix_spawn` with `POSIX_SPAWN_CLOEXEC_DEFAULT` on macOS, an explicit handle list on Windows,
  `close_range` before exec elsewhere). The parent creates its channel ends close-on-exec atomically where
  possible (`SOCK_CLOEXEC`). On macOS, it sets `FD_CLOEXEC` immediately and accepts the window that
  remains, documented.
- **Parent death:** on Linux the supervisor thread spawns, so `PR_SET_PDEATHSIG(SIGKILL)` holds for the
  process's lifetime. FreeBSD uses `procctl(PROC_PDEATHSIG_CTL)`. Windows uses a kill-on-close job object.
  macOS has no equivalent; there, a compromised helper that ignores EOF lives on until it next touches its
  dead channel. Documented, with resources capped (below).
- **Lockdown result:** the helper reports lockdown success or failure as its first PDU, never through an
  exit status. Failure refuses the extension with `HostUnavailable`, logs once at error level, and opens
  the breaker.
- **Resource caps:** `RLIMIT_NPROC=0` (Linux, FreeBSD); a job object with memory and process limits on
  Windows; an address-space/data cap sized to the reservations on POSIX where the OS can express it
  (builder measures what Wasmtime's reservations need). CPU is bounded by the call deadline and the kill.
- **Teardown off the GUI thread.** Dropping a module or an instance only *posts* to the supervisor and
  returns at once. The helper holds no state worth a graceful exit, so the supervisor closes the channels,
  kills the process at once and reaps it asynchronously. Coverage builds give a short grace period so
  `.gcda` files get written **[call]**.
- **Live upgrade of the daemon:** helpers are not handed over. The new daemon respawns them lazily.

### 5.4 Channels, protocol, guest memory

- **Channels** **[call]**: one **lane channel per worker lane**, bound at `enterThread()`, plus one control
  channel. Each lane channel is served by one helper executor thread, so no call ids and no reader
  threads are needed, and a round trip costs two wake-ups instead of four. A call waiting on `run-command`
  holds only its own lane. Transport: Unix `socketpair` or a Windows pipe pair.
- **Protocol:** versioned frames, checked at handshake (parent and helper ship together, so a mismatch is
  fatal). Encoding is dependency-free and table-tested in the style of `vthost/proto/`. Every PDU kind is
  one row: kind, direction, payload decoder, **size cap** (descriptions ≤ the module's own counts,
  artifacts ≤ 4× `maxModuleBytes`, the rest from §3's limits). Requests mirror the seam. During a `Call`,
  the helper may send `CallHost` and the parent answers `HostResult`. These nest on the same lane.
  `cabi_realloc` lowering is an ordinary nested `Call`. **Any malformed or over-cap PDU kills the helper**
  and counts a breaker fault.
- **Writes to a helper never block forever.** Lane I/O blocks, but every blocked call is bounded: the
  watchdog's interrupt, the supervisor's grace timer and its kill end it (a killed helper's channels close, which
  wakes every blocked read and write). If a helper stops reading, the call fails `HostCrashed` once the helper is
  killed. *(Revised at plan expansion, D44: same guarantee as non-blocking writes with the remaining deadline.)*
- **Guest memory, parent-authoritative** **[call]**:
  - The **parent creates** each instance's memory object and passes it down with `Instantiate`. No
    descriptor ever flows from helper to parent. Objects: a sealed `memfd` (`F_SEAL_SHRINK|F_SEAL_SEAL`)
    on Linux and FreeBSD ≥ 13; `shm_open` with a fixed name prefix, unlinked at once, on macOS
    (`ftruncate` works once, so the parent's size is final); on Windows, a `SEC_RESERVE` section.
  - **Growth goes through the parent.** The helper's memory creator turns `grow_to(n)` into a `Grow`
    PDU. The parent checks `n` against the cap, makes the range usable on its side (Windows: commits the
    pages through its view; elsewhere: nothing to do, the object is already sized), records `n` as the
    authoritative size, and acknowledges. `memory()` is bounds-checked against the **parent's** size, never
    against a size the helper reports. A hostile helper cannot make the parent touch uncommitted or
    truncated pages. This also avoids committing 32 MiB per instance up front on Windows (50 instances
    would be 1.6 GiB of commit charge).
  - In the helper, the object is mapped into a reservation with Wasmtime's required guard region. The
    bytes from the current size to the end of the reservation stay `PROT_NONE` (Wasmtime's documented
    invariant) and are opened only on an acknowledged grow. Reservation and guard sizes are set
    explicitly, not left at Wasmtime's platform defaults. Windows places the view with placeholder APIs
    (`VirtualAlloc2`/`MapViewOfFile3`). Wasmtime's memory-creator callback is engine-wide and carries no
    instance identity, so the object to use is handed to it per `Instantiate` through a thread-local set
    by the executor.
  - **When the parent touches guest memory:** only while the instance is quiescent from the parent's
    point of view: no call in flight, or the call parked in `CallHost`. Arguments lowered before `call()`
    fall under the first case.
  - **Copy-before-parse:** every byte the host validates or decodes is copied once into host-owned
    storage and never re-read from the mapping. **[call]** `RemoteBackend::memory()` returns the mapping
    for **writes and bulk copies**, and `CanonicalAbi`/`HostBindings` gain a single-fetch read helper that
    all decoding goes through. Lint: no decoder reads a guest span twice (a review rule, plus the
    hostile-memory test).
  - **Copy path** (an engine that cannot use a provided object): `memory()` reads fetch ranges over the
    control channel. Writes are buffered and **flushed before every `Call` and every `HostResult`**, so
    lowering outside host calls is covered.

### 5.5 Deadlines, interruption, timeouts

- **`interrupt()` is wait-free.** It sets the instance's sticky flag in a small shared control page (which
  the parent also creates) and does a **non-blocking one-byte write** to the helper's wake pipe; a full
  pipe means a wake is already pending. It does no channel I/O and takes no lock, so the watchdog, which
  holds the lanes' mutex, cannot be stalled by a helper.
- **In the helper:** Wasmtime runs with epoch interruption. Each call sets its epoch deadline to
  *current + 1*. The control thread, blocked on the wake pipe, increments the engine epoch. The epoch
  callback checks the instance's flag: set → trap (`Interrupted`, and every later call fails without
  entering the guest); clear → extend and continue. **No ticker, so an idle helper never wakes.**
- **Backstop:** at `interrupt()`, the supervisor arms a grace timer for that call (≈ 50 ms, tunable). The
  timer **pauses while the parent is serving a `CallHost`** for that call, since the parent is the slow
  one then. If the call has not returned when it fires, the supervisor kills the helper.
- **Non-call timeouts**, in `BackendConfig`, run by the supervisor: `Load` (compile) 10 s, `Instantiate`
  1 s, the handshake and lockdown 2 s **[call]**. An `instantiate()` that respawns runs
  handshake + `Load` + `Instantiate`, each with its own timeout. On expiry: kill, breaker fault,
  `HostUnavailable`.
- **A cold compile never runs on the GUI or daemon owner thread.** On master, status-line loading already
  happens on worker lanes; the builder verifies this. On the branch, `DaemonExtensionRuntime`'s
  synchronous load moves to a worker lane as part of the orchestrator's rebase (§8).

### 5.6 OS sandbox

**Order matters (threads):** seccomp, `no_new_privs` and Landlock are per-thread.

1. main: create every helper resource. That includes the engine, the WASI shim and the signal/trap
   machinery (Wasmtime's per-thread `sigaltstack` is set up by a priming call on every executor). The
   control and executor threads are created **after** step 2 so they inherit it.
2. main: `prctl(PR_SET_NO_NEW_PRIVS)`, then Landlock (deny all file system access, best ABI available).
   Threads created afterwards inherit both.
3. main: create the executor and control threads, and prime each.
4. main: install seccomp with **`SECCOMP_FILTER_FLAG_TSYNC`**. A TSYNC failure is a lockdown failure.
5. Report lockdown in the first PDU. The sandbox test asserts forbidden operations fail **from an
   executor thread**.

| Platform | Mechanism |
|---|---|
| Linux | As above. Default action kills the process. The allow-list table includes what Wasmtime needs after lockdown: `mmap`/`mprotect`/`munmap`/`madvise` (PROT_EXEC on private anonymous mappings; MAP_SHARED only on the received memory objects), `membarrier` (aarch64 icache coherence), `sigaltstack`, `rt_sigreturn`, `recvmsg`/`sendmsg`/`read`/`write` on its channels, `futex`, `clock_gettime`, `getrandom`, `exit_group`. Run the sandboxed contract suite on the arm64 CI leg. No namespaces, so it works unprivileged and in Flatpak. |
| FreeBSD | `cap_enter()` with rights-limited descriptors, plus `RLIMIT_NPROC=0` (capability mode still allows `fork`/`pdfork`). |
| macOS | Seatbelt, deny-default profile, through hand-declared `sandbox_init_with_parameters` as Chrome does (`sandbox_init` is deprecated, and `-Werror` forbids muting that). The profile allows `mmap` of POSIX shm by the fixed name prefix and dynamic code. Wasmtime's `macos_use_mach_ports` is **off**, so traps are plain signals with no extra handler thread **[call]**. **Entitlement [decided]:** Wasmtime does not use `MAP_JIT` (open upstream issue #11989), so `allow-jit` is not enough. The helper, and only the helper, carries `com.apple.security.cs.allow-unsigned-executable-memory`. `scripts/macos-bundle.py` gains per-helper entitlements, and CI runs the contract suite against an ad-hoc hardened-signed helper. |
| Windows | **AppContainer** (mandatory: a restricted token still reads user files and reaches the network), plus a job object (no child processes, kill-on-close, memory limit). ACG and other dynamic-code mitigations stay off for this process only. AppContainer needs read/execute on the helper's directory. **The parent ensures that grant at first spawn** (per-user MSI, zip, and build-directory test runs all lack it otherwise), as a one-time, idempotent ACL add for `ALL APPLICATION PACKAGES` on the helper binary's directory **[call]**. |

**WASI is ours, not Wasmtime's** **[call]**. The helper never calls `wasmtime_linker_define_wasi`, whose p1
implementation runs I/O through a lazily created multi-thread Tokio runtime (threads and epoll after
lockdown). The helper defines the allow-listed WASI p1 functions itself as linker functions (15 on master,
16 on the branch): no arguments, no environment, no preopens, stdout/stderr writes that reach nobody,
clocks, and `random_get`. This also makes WASI behave identically under both engines. A contract case
calls every allow-listed function inside the sandboxed helper.

**Sanitizer and coverage builds** **[call]**:
- Wasmtime's per-thread sigaltstack breaks ASan unless Wasmtime itself is built with ASan, which the
  prebuilt is not. So in sanitizer builds the helper runs `TrapMode::Software`: no signal handlers, no
  sigaltstack, explicit bounds checks.
- LSan's stop-the-world needs `clone`/`ptrace`, which the sandbox forbids, so the **real helper runs with
  leak detection off**. Leak coverage of the helper's code comes from the in-process `HostServer` tests
  (§7).
- Coverage builds add `.gcda` writes through a pre-opened directory descriptor.
- Every such widening is a separate, compile-time row set in the allow-list table. Sanitizer builds also
  give the helper a diagnostics descriptor (stderr to the parent's log). Release builds have none.

### 5.7 Engine configuration and version policy

- **Feature set = exactly what the guests and the WAMR fallback support**, in one table shared by both
  engine configs and the module world check **[call]**: MVP + bulk-memory, sign-extension,
  saturating float-to-int, mutable globals, multi-value, reference-types (current Rust emits it), extended-const (Wasmtime 48 cannot switch it off; ruling R6).
  Explicitly **off**: GC, exceptions, threads, memory64, multi-memory, tail calls, SIMD, component model.
  A module needing more is refused at load with the same error under both engines. Smaller attack surface,
  and no per-platform differences in which modules are accepted.
- `TrapMode::Signals` in release helpers and `Software` in sanitizer builds and in in-process tests.
  Cranelift `opt_level=speed`, parallel compilation off.
- **Version:** pin the current **LTS** release (every 12th major gets 24 months of security fixes),
  presently v48, and move LTS to LTS. Security advisories in between are taken as patch releases **[call]**.
- **Tiers:** Linux x86_64 Tier 1. Linux aarch64 and Apple Silicon Tier 2, which is acceptable. FreeBSD
  x86_64 Tier 3 → FreeBSD ships the WAMR engine in the helper (still out of process and sandboxed) until
  that changes **[call]**.

### 5.8 Getting Wasmtime, packaging, caching

- **Build:** only `contour-extension-host` links Wasmtime, statically, from the pinned prebuilt
  `c-api` archive (version + SHA-256 per platform) when `CONTOUR_USE_CPM=ON`. Otherwise a system
  `wasmtime` found by a probe that checks version and features. Otherwise configure falls back to the WAMR
  engine, with a status line naming the reason. The `check_wasm_runtime_containment` lint learns two
  rules: the new backend directory, and that **the GUI and daemon targets link no engine at all**.
- **Size:** ~30 MB shared / 68 MB static archive. The stripped static helper is to be measured and stated
  in the PR.
- **Per package** **[decided]**:

| Package | Engine in the helper |
|---|---|
| Upstream CI artifacts: DEB, AppImage, macOS DMG, Windows MSI/zip | Wasmtime (prebuilt archive) |
| Flatpak (offline) | Wasmtime as a manifest module from the same prebuilt archive, pinned by SHA-256 as a source. If Flathub rejects prebuilt binaries, build it from source with `flatpak-cargo-generator` sources. |
| Fedora RPMs `contour` + `contour-daemon` (Koji, offline) | WAMR in the helper until Wasmtime is vendored/packaged, the same follow-up as the guests' crates (`contour.spec:113`) |
| FreeBSD | WAMR in the helper (Tier 3, §5.7) |

- **Install location:** libexec on Unix, next to `contour.exe` on Windows, `Contents/Helpers/` on macOS.
  The parent finds it relative to its own executable.
- **Compile cache (Wasmtime only):** the parent owns it. After a compile, the helper returns the serialized
  artifact (capped, §5.4). The parent stores it atomically with user-only permissions, keyed by module
  SHA-256 + Wasmtime version + engine config + CPU features, in a size-capped LRU (256 MiB) **[call]**.
  Deserializing is trusted code loading, but it runs inside the sandbox, so a tampered entry compromises
  only the already-contained helper.

### 5.9 Every change above the seam (G6)

1. `ExtensionRegistry` + breaker, and the status line loading once per process (§5.2).
2. `ModuleIdentity` passed to `load` (§5.1).
3. `HostUnavailable` passed through `CompiledExtension::load` (§5.1).
4. A single-fetch read helper in `CanonicalAbi`/`HostBindings` (§5.4).
5. `makeDefaultBackend(BackendConfig)` at the composition roots (GUI, daemon, CLI, `bench-headless`, tests).
6. Branch only, at its rebase: the daemon's module load leaves its owner thread.

## 6. Documentation

`docs/extensions.md`: "The WebAssembly runtime" and "Performance" become "The extension host process"
and its sandbox. The engine is not named in user-facing text. `docs/internals/`: the rationale and the
protocol. A `metainfo.xml` release note.

## 7. Testing

- **Contract suite** per composition, each its own test binary (the singletons are link-time):
  `RemoteBackend` over an in-process `HostServer` (socketpair; deterministic; ASan/LSan/TSan-clean, so
  this is where helper-side leaks are caught), `RemoteBackend` over the real sandboxed helper (both engines),
  and `WasmtimeEngine` directly in `TrapMode::Software`. `installsNoSignalHandlers` applies to the parent
  compositions.
- **Protocol:** table-driven encode/decode, over-cap and malformed rejection, fuzzed.
- **Crash and hostility:** a test helper that aborts, segfaults, exits mid-`CallHost`, ignores interrupt,
  stops reading its channel, sends garbage, reports a bogus size, or tries to grow past the cap. Each must
  yield bounded latency, `HostCrashed`, one breaker fault, lazy recovery, and an open breaker after the
  limit. The GUI-thread-free teardown is asserted.
- **Hostile memory:** guest memory is mutated by a **separate process** through its own mapping (not a
  second thread, which would be a data race that TSan rightly rejects) while the host decodes.
- **Sandbox:** per platform, forbidden operations (open, connect, fork/spawn, ptrace) fail from an
  executor thread after lockdown. Every allow-listed WASI function works inside the sandbox.
- **Existing suites:** `Bundled_test` (master) and `DaemonGuest_test` (branch, after its rebase) pass on
  the default composition.

## 8. Delivery

- Lands on **master** in its own branch and PR, from a worktree. The status-line world is master's scope.
  `feature/agent-orchestrator` rebases afterwards and takes §5.9 item 6.
- **Gate 1 before merge** is measured on a throwaway local rebase of the orchestrator branch onto the PR
  branch, never pushed. The numbers go into the PR description.
- Suggested phasing (the builder may refine):
  1. `IHostedEngine` with `WasmtimeEngine` in-process, in `TrapMode::Software`, plus our WASI shim.
  2. Protocol, `HostServer`, `RemoteBackend` over a socketpair, and the registry/breaker.
  3. The executable, supervisor, launcher and process handles.
  4. Shared memory and parent-authoritative growth.
  5. Sandboxes, Linux first.
  6. The compile cache, packaging, docs and the gates.

## 9. Measurement gates (before merge)

1. Orchestrator `update(procs)` on `prs-100.json`, daemon-world limits, through `RemoteBackend` with
   the real sandboxed helper: ≥ 5× faster than the 35 ms median. Report min/median/max over three release
   runs.
2. **Whole-callback IPC overhead** (the same callback, helper vs `HostServer` in-process), plus per-round-trip
   latency for information. The target is overhead ≤ 10 % of the callback.
3. The status-line performance table in `docs/extensions.md`: no regression beyond noise. Idle wake-ups
   with five extensions configured: unchanged from today.
4. RSS for 1 and 10 panes with all five bundled extensions, parent + helpers summed, against today's
   13.2 / 104.2 MiB. Any growth is explained in the PR.
5. Cold compile and warm (cached) load per bundled extension, against the `Load` timeout (§5.5).

## 10. Owner decisions (were **[confirm]**; both accepted by Christian, 2026-11-24)

1. **macOS entitlement:** `allow-unsigned-executable-memory` on the helper only. The alternatives are a
   patched Wasmtime that uses `MAP_JIT`, or WAMR on macOS.
2. **Offline distro packages** (Fedora RPMs, and Flatpak if Flathub refuses the prebuilt archive) ship the
   WAMR engine in the helper: as isolated, but not faster, until Wasmtime is vendored.

## 11. Left to the builder

PDU layout. Directory and module names under `src/vtextension/` (the user-facing name is fixed). Grace and
timeout values within the stated orders of magnitude. The POSIX memory-cap mechanism. Whether WAMR can
place linear memory into a provided object (otherwise the copy path). Exact Seatbelt operation names.
Windows placeholder-API details.
