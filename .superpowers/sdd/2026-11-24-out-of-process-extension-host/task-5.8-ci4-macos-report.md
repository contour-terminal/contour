# Task 5.8 — CI round 4, macOS: report

Status: DONE_WITH_CONCERNS (no Mac; the macOS-only lines could not be compiled; see "What was and wasn't compiled").

Base ce9062c11. New commits (not pushed). The fixes were made as new commits, not folded into the commits that
introduced the code: each fix touches code from several older commits (8f0ba0a2b, a264a8f9d, 162847b1e, 8e2f44968,
a66c69ead, bcb5c5142) that later commits rewrote in the same places, across a 174-commit branch. An autosquash would
risk conflicts and an unbuildable middle.

| Commit | Subject |
|---|---|
| 7e685b25a | fix(vtextension): an extension host's channel never waits to send on macOS |
| bcbcfd69b | fix(vtextension): a descriptor that comes unasked is closed on macOS |
| 09e38993f | fix(vtextension): WAMR maps memory MAP_JIT only where it is to run code |
| f2d60e683 | fix(vtextension): the macOS sandbox lets the extension host learn its page size |
| ff4e3d5e1 | test(vtextension): the macOS sandbox probe expects what Seatbelt refuses |

## Root causes

### 3. vtextension_test timeout (600 s): `PduStream.trySendKeepsAPartialFrame` hung (production defect)
- Evidence: the log's trace (lines ~7900-8690) has 253 `case:` lines and 252 `ended:` lines. The only case that
  started and never ended is `PduStream.trySendKeepsAPartialFrame`.
- Cause: `Channel::trySend()` (Channel_posix.cpp) relied on `send(..., MSG_DONTWAIT)`. XNU ignores MSG_DONTWAIT on a
  send. `sosendcheck()` returns EWOULDBLOCK only for `SS_NBIO` (a non-blocking socket), `MSG_NBIO` (kernel-private) or
  `assumelock`. `sendit()` passes the flags through unchanged. This was checked against
  apple-oss-distributions/xnu bsd/kern/uipc_socket.c and uipc_syscalls.c. The test's fill loop, with nobody reading,
  therefore blocked forever.
- Production impact: `HelperIncarnation::offerReleases()` → `trySendReleases()` could block a thread of Contour's
  while the control channel was full. Its design says it never waits.
- Fix: on macOS (`DontWaitSend::Ignored`), each send goes out only once `poll(POLLOUT, 0)` says the socket is
  writable, which means room for at least its `SO_SNDLOWAT`. Each send is at most that low-water mark, which
  `sosendcheck()` takes without waiting.
  - Only one thread sends on an end at a time (the control mutex), so the room only grows in between.
  - On POLLHUP or POLLERR the send runs and reports the error.
  - Linux and FreeBSD (`Honoured`) are unchanged.
  - O_NONBLOCK was rejected: it would make a receive that another thread blocks in fail.
- Test: the fill loop and the blocking send now run under 5 s deadlines in threads of their own, with abort plus a
  message, like the existing `Channel.trySendNeverBlocks`. A regression now fails the case instead of hanging the
  binary.

### 2. `PduStream.lanesNeverInstallADescriptor` 6 vs 5 (and `tooManyDescriptorsAreMalformed` 7 vs 6) (production defect)
- Evidence: log lines 7936-7953 and 8501-8510. Both counts rose by one, and the baseline stayed one higher afterwards,
  so this is a real leak, not a lazily created OS descriptor.
- Cause: XNU's `soreceive_ctl()` externalizes SCM_RIGHTS (installs the descriptors) whether or not the caller asked
  for control data. With `controlp == NULL` it then just frees the message, so the descriptor stays installed with no
  name. A truncated receive behaves the same way: every descriptor is installed before copyout truncates the control
  data (`copyout_control` only sets MSG_CTRUNC). Source: xnu uipc_socket.c `soreceive_ctl`, uipc_syscalls.c
  `copyout_control`.
- Production impact: on macOS, each frame a compromised helper sent with a descriptor would have cost Contour one
  descriptor permanently. Contour's ends use plain receives, and macOS has no SO_PASSRIGHTS.
- Fix (data-driven `UnclaimedDescriptors::{Disposed, Installed}` per system):
  - On macOS, every receive goes through one `recvmsg()` helper with a control buffer big enough for anything a
    message can carry. XNU takes at most MCLBYTES (2048) of control data in, after widening each descriptor to a
    pointer, so 2048 user bytes always suffice.
  - `receiveSome()` closes whatever came. `receiveSomeWithHandles()` hands every descriptor to the caller, which
    refuses more than one as before (Malformed, all closed).
  - Linux and FreeBSD keep a plain `recv()` and the two-descriptor buffer.
  - The descriptor loop is now bounded by the received control length rather than `MaxReceivedHandles`.
  - Docs updated: the Channel.hpp `receiveSome` and `receiveSomeWithHandles` comments, `UpwardDescriptors`, and a
    Hostility_test comment.

### 4. vtextension_wamr_helper_test: 34/62 failing at instantiate (production defect)
- Evidence: the job's "sandbox log" step (log lines ~10275-10370) shows, for every WAMR helper process,
  `Sandbox: contour-extension-host-wamr(N) deny(1) sysctl-read hw.pagesize_compat`. It shows no other denial.
  - Loads succeeded, so the module text was accepted. The cases that expect an instantiation refusal passed.
  - Every real instantiate failed, and the user-facing text was `⚠ git.wasm disabled: invalid WebAssembly module`,
    which is InvalidComponent from `wasm_runtime_instantiate_ex2` failing.
- Cause: WAMR's `os_mmap()` (core/shared/platform/common/posix/posix_memmap.c) calls `getpagesize()` as it maps an
  instance's linear memory. That happens after the lockdown, because WAMR uses Copy mode and maps memory itself.
  - On macOS, libc asks the kernel for `hw.pagesize_compat` until the call succeeds. The denied read made it return
    ≤ 0, `request_size` became 0, and os_mmap returned NULL.
  - The Wasmtime helper is unaffected in that path; its memory is provided.
- Fix: the profile's single `sysctl-read` rule now also allows `(sysctl-name "hw.pagesize_compat")`.
  - It is one exact name in the existing rule, so `Sandbox.profileHasNoNetworkOrMachLookup` still sees one form per
    operation, and the test now checks for the new filter.
  - The probe's `permitted` op calls `getpagesize()` after the lockdown on every system, as such an engine does.
- Second cause, fixed ahead of the next round (09e38993f): WAMR 2.4.5's `os_mmap()` maps **everything** with MAP_JIT
  on macOS arm64.
  - The WAMR helper is signed with the hardened runtime and no `allow-jit` entitlement (correctly: it interprets), and
    Apple requires that entitlement for MAP_JIT under the hardened runtime.
  - Once the page size is answered, that mapping would most likely be refused next. This is not visible in this log,
    because os_mmap bailed out before reaching mmap.
  - New patch `cmake/patches/wamr-map-jit-only-for-executable-memory.patch` sets MAP_JIT only when PROT_EXEC is
    requested, which the interpreter never does. Non-Apple builds are unchanged.
  - `ContourThirdParties.cmake` now applies a list of patches: one ApplyPatchOnce `COMMAND` per patch, and all
    patches' hashes in `CUSTOM_CACHE_KEY`. Verified locally: a reconfigure of an existing tree applied the new patch
    and skipped the old one, and the patched WAMR builds and passes on Linux.
  - Diagnostic for the next run: a macOS-only `map-jit` probe row (Unasserted, so recorded as a WARN). It shows what
    the sandbox does with a MAP_JIT mapping in an unsigned probe.

### 1. `Sandbox.forbiddenOperationsFailFromAnExecutor` (test expectations)
- `execve`, refused:2 (ENOENT): **test defect.** The probe ran `/bin/true`, which macOS does not have. It now runs
  `/usr/bin/true` on macOS (`RunnableProgram`) and still `/bin/true` elsewhere. The expected EPERM comes from
  Seatbelt's process-exec denial.
- `socket-inet` / `socket-unix`, "ok": the lockdown was applied. In the same probe run, the profile refused
  open-file, open-directory, connect-inet, fork, signal-parent and mach-lookup with the expected errors (only the
  three rows failed), and Seatbelt is per process, so the executor thread is covered.
  - So under this `(deny default)` profile on the macOS 15 runner, creating an AF_INET or AF_UNIX stream socket is
    simply not refused.
  - I could not find an Apple primary source that settles why. Third-party profile generators (e.g. nono, zerobox)
    emit `system-socket` rules as if creation were mediated, but the run contradicts that for these domains.
  - Expectations changed, with the reason stated in the table's comment, matching FreeBSD's: socket-inet and
    socket-unix are `Allowed` on macOS. A socket reaches nothing without an address, and every address is refused.
  - A new `connect-unix` row (a connect to `/var/run/syslog` by path) is asserted on Linux (EPERM at socket
    creation), FreeBSD (ECAPMODE) and macOS (EPERM, network-outbound), next to connect-inet. No refusal was loosened.
- `protect-writable-executable`, refused:13: only a warning (Unasserted), and unchanged.

### "verify … actually ran" steps
The macOS job's verify steps (tmux oracles, shell integrations, menu bar) do not rely on the vtextension suites.
The WAMR/Wasmtime helper step does:
- It runs `bash -e`, so the WAMR failure ended it before the Wasmtime helper suite and the unentitled-kill check ever
  ran on macOS.
- They will run for the first time once WAMR passes. See the concerns.

## Tests (Linux, clang-asan, at HEAD ff4e3d5e1)
- vtextension_test: all passed, 486 cases / 26718 assertions.
- vtextension_process_test: 141 cases, 140 passed, 1 skipped.
- vtextension_helper_test: 161 cases, 160 passed, 1 skipped.
- vtextension_wamr_helper_test: all passed, 62 cases / 1237 assertions. This is the patched WAMR.
- vtextension_remote_test: all passed, 302 cases.
- vtextension_remote_wamr_test: 55 cases, 54 passed, 1 skipped.
- ctest -L lint: 35/35.
- macOS code paths exercised on Linux: I temporarily switched `UnclaimedDescriptors` and `DontWaitSend` to the macOS
  values, rebuilt and ran vtextension_test. All Channel.* and PduStream.* cases passed (29 cases), and so did all
  `[vtextension]` cases (387). The constants were reverted afterwards.
- clang-tidy 22 (/usr/bin/clang-tidy) on Channel_posix.cpp, PduStream_test.cpp, SandboxProbe.cpp,
  SandboxProbe_test.cpp and Sandbox_test.cpp: no findings. The build tree has `ENABLE_TIDY=OFF`, so tidy was run by
  hand.
- clang-format 22.1.8 `--dry-run --Werror` and editorconfig-checker: clean.
- Commit A's state was built and its Channel/PduStream cases run on their own. B through E were tested as the final
  tree, which is byte-identical to what was tested.

## What was and wasn't compiled
- Compiled on Linux: all of Channel_posix.cpp, including the macOS branches. They are `if constexpr` in non-template
  functions, so both arms are compiled, and they were run with the constants switched.
- Compiled on Linux: the profile text, which Sandbox_test reads on every system, and the patched posix_memmap.c
  (non-Apple arm).
- **Not compiled:** the `#ifdef __APPLE__` lines:
  - `RunnableProgram = "/usr/bin/true"`, a trivial constant.
  - `mapForJit` (MAP_JIT exists only in macOS headers).
  - The macOS expectation table.
  - The Apple arm of the WAMR patch (`map_flags |= MAP_JIT;`, moved verbatim from upstream's own `#if`).
- `connect-unix` and the `sys/un.h` include are platform-free and were compiled.

## Concerns
1. macOS socket creation is accepted as allowed on the strength of one CI run plus the reasoning above. If a later
   macOS refuses it, the `Allowed` rows fail loudly, which is the desired signal.
2. The connect-unix EPERM on macOS assumes Seatbelt checks `mac_socket_check_connect` before the path lookup (as
   network-outbound). The FreeBSD ECAPMODE row is new and unverified on FreeBSD, though capability mode refuses any
   connect() with an address.
3. The WAMR MAP_JIT refusal under the hardened runtime is inferred from Apple's allow-jit documentation, not observed.
   The patch removes the question either way, and the `map-jit` probe row records the sandbox side next run.
   - A system or Homebrew WAMR (the find_package path) goes without the patch and would still map MAP_JIT on macOS.
4. The Wasmtime helper suite and the "unentitled helper is killed" check have never run on macOS under the lockdown.
   They run next round, after the WAMR suite, and may surface new denials. The sandbox-log step will show them.
5. The probe's own sandbox denials never appeared in the job's deny log (only WAMR helper lines did), even though
   its file opens were refused with EPERM. The predicate matches `extension_host_p`. Worth a look if the log is to be
   relied on for the probe.
6. Before this change, the last assertion of `Channel.trySendNeverBlocks` (trySend is Closed after the peer's
   `shutdown()`) had never run on macOS (vtextension_test hung first). XNU's `sorflush()` may drop rather than refuse
   data to a read-shut-down peer, so that line could be the next macOS finding. It is independent of this change.
7. On macOS, received descriptors are briefly installed without FD_CLOEXEC until the fcntl, as before, because macOS
   has no MSG_CMSG_CLOEXEC. receiveSome() now closes them at once, so the window is short but nonzero.
8. No diagnostic carries WAMR's instantiate error text into the test failure message. That would mean threading text
   through WamrInstance, the protocol and the backend. Making the helper's stdout (where WAMR prints) its stderr was
   rejected because stdout is a deliberately idle descriptor (IdleStream, limited to fstat on FreeBSD). The job's
   sandbox-log step was what identified this round's denial, and it stays the diagnostic.

## Round 1 (review of ce9062c11..ff4e3d5e1)

Every finding was folded into the five commits with fixup and autosquash, and the messages were updated. The commit
hashes changed:

| Commit | Subject | Round-1 content |
|---|---|---|
| 7b5d1cbd3 | channel never waits to send on macOS | item 3 |
| 410012ba0 | a descriptor that comes unasked is closed on macOS | items 5 and 6 |
| eb4e5f294 | WAMR maps memory MAP_JIT only where it is to run code | item 4 |
| 09f559217 | the macOS sandbox lets the extension host learn its page size | item 2 |
| a954a9112 | the macOS sandbox probe expects what Seatbelt refuses | item 1 |

HEAD is a954a9112 (not pushed).

1. **Important: what an allowed socket is still refused.** New probe ops in SandboxProbe.cpp:
   - `sendto-inet`: an unconnected UDP socket sending to 127.0.0.1:9.
   - `listen-inet`: bind to loopback, then listen.
   - `bind-unix`: bind to a `/tmp/contour-sandbox-probe-<pid>.socket` path, unlinked if the bind ever succeeds.
   - `socket-route`: a socket(AF_ROUTE, SOCK_RAW).
   - `socket-system`: AF_SYSTEM with SYSPROTO_CONTROL; macOS only.
   - `list-interfaces`: SIOCGIFCONF on an AF_INET datagram socket.

   Expectations per system:
   - **macOS:** sendto-inet, listen-inet and bind-unix are refused with EPERM, asserted.
     - XNU calls `mac_socket_check_send` and `mac_socket_check_bind` before the protocol, as it calls
       `mac_socket_check_connect`. Seatbelt maps these to network-outbound and network-bind, which `(deny default)`
       covers.
     - connect-inet was refused with EPERM this way in the observed run.
   - **macOS:** socket-route, socket-system and list-interfaces are Unasserted. Whether Seatbelt asks the profile
     about other families' sockets and about interface ioctls is unknown, so the next log will report it.
   - **Linux:** all six ops are refused with EPERM, because seccomp refuses `socket` unconditionally
     (SystemCallRows.cpp:241) and each op fails at its socket() call.
   - **FreeBSD:** sendto-inet, listen-inet and bind-unix are refused with ECAPMODE (an address in capability mode).
     socket-route and list-interfaces are Unasserted because I am not sure what Capsicum does with them.
   - The macOS table comment now says exactly what the rows prove, and that the other families and the interface
     listing are recorded only.
2. **Page-size check.** On macOS the probe's `permitted` op now asks `sysctlbyname("hw.pagesize_compat")` directly
   after the lockdown, so the row guards the profile rule itself. A getpagesize() answer cached before the lockdown
   can no longer stand in. Other systems still call getpagesize().
3. **Sender contract.** The Channel.hpp class contract now says one thread at a time sends on an end (send(),
   trySend() or sendWithHandle()) while a receive may run beside it. trySend()'s doc points to that contract.
4. **Unpatched WAMR on macOS.** ContourThirdParties.cmake now refuses an unpatched WAMR on APPLE:
   - An embedded `iwasm::vmlib` stops the configure with FATAL_ERROR and the reason.
   - A system (find_package or pkg-config) WAMR is rejected with that reason. With CONTOUR_USE_CPM, the fetched,
     patched WAMR is used instead; without it, the existing FATAL_ERROR fires and names the reason.
   - The check covers all of APPLE, not arm64 only. Upstream adds MAP_JIT only on arm64, but universal or cross builds
     make the architecture test unreliable.
   - Only the non-APPLE path was executed (Linux reconfigure). The APPLE branches were parsed but not run.
5. **Broken doc sentence.** The reflowed `UpwardDescriptors` sentence in Channel.hpp is fixed.
6. **Truncated or foreign ancillary data.** On macOS, `receiveSome()` now writes one warning per process when a
   receive brought ancillary data other than descriptors, or more than a message carries (MSG_CTRUNC). Everything
   that came is closed, and the stream goes on, as where the system discards. The warning goes to the module's
   `vtextension.warning` category.
   - core::log asserts that category names are unique, so the category moved from HostRefusals.cpp's anonymous
     namespace into `src/vtextension/wasm/remote/WarningLog.{hpp,cpp}` in vtextension_remote_wire.
   - vtextension_remote_wire now links core::log publicly, and HostRefusals uses the shared object.

### Round-1 tests (Linux clang-asan, HEAD a954a9112)
- vtextension_test: all passed, 486 cases.
- vtextension_process_test: 141 cases, 140 passed, 1 skipped. The new probe rows ran on Linux.
- vtextension_helper_test: 161 cases, 160 passed, 1 skipped.
- vtextension_wamr_helper_test: all passed, 62 cases.
- vtextension_remote_test: all passed, 302 cases.
- vtextension_remote_wamr_test: 55 cases, 54 passed, 1 skipped.
- ctest -L lint: 35/35.
- macOS channel paths forced on Linux again (both constants switched, then reverted): Channel.* and PduStream.*
  passed, 29 cases.
- Commit 410012ba0 was built on its own (vtextension_test, vtextension_process_test, contour-extension-host).
- clang-tidy 22 on Channel_posix.cpp, SandboxProbe.cpp, SandboxProbe_test.cpp, WarningLog.cpp and HostRefusals.cpp:
  no findings. clang-format 22 `--dry-run --Werror` and editorconfig-checker: clean.

### Round-1: not compiled locally
The macOS-only additions were not compiled locally:
- `socketSystem` (needs `AF_SYSTEM`/`SYSPROTO_CONTROL` from `<sys/sys_domain.h>`).
- The `sysctlbyname` call and the `<sys/sysctl.h>` and `<sys/sockio.h>` includes.
- The FreeBSD `<sys/sockio.h>` include.

### Round-1 concerns
- sendto-inet, listen-inet and bind-unix are asserted refused on macOS from XNU's MAC hook placement, not from
  observation. A failure there next run means Seatbelt does not mediate that call, which is itself the finding.
- The FreeBSD rows for these three are new and unverified on FreeBSD. ECAPMODE for an address in capability mode is
  well established.
- `vtextension_remote_wire` now depends on core::log. The helper links it too: on macOS the channel can log from
  inside the sandboxed helper, writing to descriptors it already holds, which Seatbelt does not mediate. In practice
  that happens only if Contour itself sent foreign ancillary data.
- APPLE now rejects every installed WAMR, including one that may already carry an equivalent upstream fix. When
  upstream WAMR gains the change, this should become a version check.

## Round 2 (re-review of a954a9112)

The three minors are folded into their commits with fixup and autosquash, and HEAD is ffd1e0a77 (not pushed). The
commits from 214575c63 on changed hash; 7b5d1cbd3 is unchanged.

| Commit | Round-2 content |
|---|---|
| 03ecc070e | minor 3 (descriptor closing) |
| 214575c63 | minors 1 and 2 (MAP_JIT patch) |

1. **CPM-off advice on macOS.** With CPM off, the FATAL_ERROR now takes its remedy from `_wamr_remedy`. On APPLE
   that is "Set CONTOUR_USE_CPM=ON, or configure with -DCONTOUR_EXTENSIONS=OFF", because an installed WAMR is
   refused there. Elsewhere the remedy is unchanged, "Install one, …". The APPLE branch was checked with a cmake -P
   snippet of the same logic; only the non-APPLE path ran in a real configure.
2. **Refusal wording.** The comment and the refusal text now say the MAP_JIT mapping happens on Apple silicon, and
   that an installed or embedded WAMR cannot be told apart from a patched one. That is why every Apple build refuses
   it, Intel included, since one package may serve both. This matches the CPM comment.
3. **Test for the warn-once discard.** The macOS receive is now a public POSIX function,
   `transport::receiveSomeClosingAttachments(Channel&, span)` (Channel.hpp).
   - `Channel::receiveSome()` calls it on macOS. It always uses the 2048-byte whole-message control buffer, so it
     behaves the same on any POSIX system. `receiveMessage()` now takes its control buffer from the caller.
   - New `Channel.foreignAncillaryDataIsDiscardedAndToldOnce` runs in vtextension_test on Linux, which CI runs. No
     forced-mode build target is needed. The test:
     - sets SO_PASSCRED on the receiving end;
     - sends two messages, the first with a descriptor attached;
     - checks that each byte arrives and the open-descriptor count is unchanged;
     - captures the `vtextension.warning` category's sink (looked up in core::log's registry, restored by
       core::Finally) and checks for exactly one warning.
   - The once-flag is per process, and only this test reaches the warning on Linux, so the count is deterministic.

### Round-2 tests (Linux clang-asan)
- vtextension_test: all passed, 487 cases (one new).
- vtextension_process_test: 141 cases, 140 passed, 1 skipped.
- vtextension_helper_test: 161 cases, 160 passed, 1 skipped.
- vtextension_wamr_helper_test: all passed, 62 cases.
- vtextension_remote_test: all passed, 302 cases.
- vtextension_remote_wamr_test: 55 cases, 54 passed, 1 skipped.
- ctest -L lint: 35/35.
- Commit 03ecc070e was built on its own (vtextension_test).
- clang-tidy 22 on Channel_posix.cpp and Channel_test.cpp: no findings. clang-format 22 and editorconfig-checker:
  clean.

## Round 3 (re-review of ffd1e0a77)

The four items are folded into their commits, and HEAD is c66b53a20 (not pushed). Item 3 is in bb6a2b3dc; items 1,
2 and 4 are in 8cab68cbe.

1. **Channel.hpp is the channel's API only again.** `receiveSomeClosingAttachments()` moved out of it into
   `transport::testing`.
   - It is declared in the new `transport/testing/ClosingReceive.hpp`, beside HandleCopies.hpp, and still defined
     in Channel_posix.cpp.
   - In production the macOS receive is an internal function, `receiveClosingAttachments(int, span)`, in an
     anonymous namespace. `Channel::receiveSome()` calls it on macOS, and the testing function forwards to it.
2. **Once-per-process warning.** vtextension_test is a single process: one `add_test(NAME vtextension_test …)`,
   no catch_discover_tests. So the test no longer relies on running first.
   - The once flag is now a resettable `std::atomic<bool>` behind a function-local accessor.
   - The testing header's `forgetToldForeignAncillaryData()` resets it. The case calls it after capturing the sink,
     and the exactly-one assertion carries a comment saying why.
3. **CMake string.** `_wamr_unpatched_refusal` is now built with `string(CONCAT …)`. Before, it was a two-element
   list that printed "under ;the hardened".
4. **No bool loop.** The bool loop selector is gone. The case makes two explicit sends (one with a descriptor, one
   with bytes only) through a `receiveOne` lambda.

### Round-3 tests (Linux clang-asan)
- vtextension_test: all passed, 487 cases.
- vtextension_process_test: 141 cases, 140 passed, 1 skipped.
- vtextension_helper_test: 161 cases, 160 passed, 1 skipped.
- Commit 8cab68cbe was built on its own (vtextension_test).
- clang-tidy 22 on Channel_posix.cpp and Channel_test.cpp: no findings. clang-format 22 and editorconfig-checker:
  clean.
