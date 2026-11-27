# Task 2.1 report: PDU codec, channels and framed streams

**Status:** DONE (one commit; nothing pushed)

**Commit:** `fbfcb6778 feat(vtextension): a bounded wire protocol and channels for the extension host process`

## What changed

New, `vtextension_remote_wire` (static; links nothing; include path `src/` only, since the vocabulary it names
is header-only and std-only):
- `src/vtextension/wasm/remote/CMakeLists.txt`: the target. Option `VTEXTENSION_FUZZING` (OFF) adds
  `vtextension_remote_fuzz` with `-fsanitize=fuzzer`. Configuring it with a non-Clang compiler is a FATAL_ERROR.
- `proto/Wire.hpp`: `DecodeError` (the brief's 9 values) and `toString`, `boundedReserveCount<T>`, `Writer`, and
  `Reader`. The reader takes a u32 varint in at most 5 bytes and a u64 varint in at most 10. A higher bit in the
  last byte is `MalformedVarint`, and a read past the end is `Truncated`.
- `proto/Pdu.hpp`: every type in the brief, verbatim, plus default member initializers (zero is the negative
  case) and defaulted `operator==`. `MaxHostParameters = wasm::MaxHostParameters`: an alias, so it cannot drift.
  Two static_asserts tie `PduLimits {}` to `ExtensionLimits {}` (D6), and one ties the variant size to `PduKind`.
- `proto/PduTable.hpp`: the 15 rows with the brief's caps. A static_assert checks that row `i` has kind `i + 1`
  and decodes `variant_alternative_t<i, Pdu>`, by comparing function pointers.
- `proto/Codec.hpp/.cpp`: `kindOf`, `encodeFrame` and `decodeFrame`.
  - Each struct's fields are listed once by a `members()` overload, which takes the struct const or not. Typed
    `write`/`read` overloads walk that list: u32/u64 varint, one byte per enum, status `0` / `value + 1`, and
    count-prefixed strings, byte vectors and vectors.
  - A `lastEnumerator()` overload per wire enum gives the `BadEnum` bound.
  - `withinCounts()` holds `Call`/`CallResult` to `MaxCallValues` and `CallHost` to `MaxHostParameters` (D6).
    A byte cap alone would let a Call hold ~5× the values.
  - Frame checks run in this order (D7):
    1. The length is checked against the largest cap + 5. A failure is `OverCap`, before the kind arrives.
    2. The kind: `UnknownKind`, then `WrongDirection`, then `WrongChannel`.
    3. The body length is checked against the row cap: `OverCap`.
    4. `NeedMoreData` until the body is buffered.
    5. The body is decoded: `TrailingBytes` unless it is consumed exactly.
- `proto/Codec_fuzz.cpp`: decodes the input for all 4 (direction, channel) pairs under small limits. Anything that
  decodes must encode again and decode to an equal PDU.
- `transport/Channel.hpp`, `Channel_posix.cpp`, `Channel_win32.cpp`: a pimpl `Channel::Native`, defined per
  platform.
  - POSIX: `socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC)`. Where `SOCK_CLOEXEC` or `MSG_NOSIGNAL` is missing
    (macOS), it uses `fcntl(FD_CLOEXEC)` and `SO_NOSIGPIPE` instead. Feature tests decide this, not OS names.
  - POSIX I/O: `send` uses `MSG_NOSIGNAL` and loops on partial writes and EINTR. `shutdown()` is
    `::shutdown(SHUT_RDWR)`, and the descriptor is closed only in the destructor.
  - Windows: `CreateNamedPipeW` with `PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE`
    and `PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS`, 1 instance. The client
    is opened with `CreateFileW(FILE_FLAG_OVERLAPPED)`. `ConnectNamedPipe` then accepts `ERROR_PIPE_CONNECTED`,
    or waits on `ERROR_IO_PENDING`. Neither handle is inheritable (null security attributes).
  - The pipe is named `contour-extension-host-<pid>-<serial>-<64 random bits>`. The random bits come from
    `std::random_device`, so no bcrypt is needed.
  - Windows I/O: one overlapped transfer per call, each with its own event. `shutdown()` sets an atomic state,
    then calls `CancelIoEx(pipe, nullptr)`. A transfer re-checks the state after it starts and cancels itself
    if needed, which closes the race with a shutdown that lands between its start and its wait.
- `transport/PduStream.hpp/.cpp`: owns the channel and a receive buffer.
  - It decodes after every `receiveSome`. That costs O(header) while the frame is incomplete, because
    `decodeFrame` returns `NeedMoreData` before it touches the body.
  - The buffer grows in 64 KiB steps, or doubles. Once it drains after a large frame, it is released.
  - `OverCap` maps to `OverCap`; every other decode error maps to `Malformed`.

Modified:
- `src/vtextension/CMakeLists.txt`: `add_subdirectory(wasm/remote)` comes before the backend's. The three
  `_test.cpp` files join `vtextension_test`, which links `vtextension_remote_wire`.
- `src/vtextension/wasm/WasmBinary.hpp`: `ImportSectionId = 2`, `DeclaredCounts` (with `==`) and
  `declaredCounts()`. It reads only the count that opens each section, 0 when absent. A second import or export
  section is `Malformed`. It follows the style of `tableDeclarations`.
- `src/vtextension/wasm/WasmBinary_test.cpp`: `WasmBinary.declaredCounts` checks `DescribedModule` → `{2, 2}`,
  plus table cases: absent, counts only, truncated count, duplicate section and not-a-module.
- `src/vtextension/wasm/WasmRuntime.hpp`: defaulted `operator==` on `ImportDescription` and `ExportDescription`.
  The round trip compares `Loaded`, so it needs them.

## Tests (TDD)

- **RED.** I wrote the tests and wired CMake, with empty `.cpp` placeholders. Building `vtextension_test` failed
  (exit 1) with `no member named 'DeclaredCounts' / 'declaredCounts' in namespace 'vtextension::wasm::binary'`,
  and the codec and transport symbols were missing.
- **Behavioural RED (mutation check).** With the codec implemented, I disabled two checks: the largest-cap header
  check and `withinCounts`. 2 of 8 PduCodec cases then failed, with 3 failing assertions: the length-only
  `OverCap`, and the Call and CallHost count `OverCap`. I restored the file afterwards.
- **GREEN:** `vtextension_test "PduCodec.*,Channel.*,PduStream.*,WasmBinary.*"` → All tests passed (5440
  assertions in 24 test cases).
  - PduCodec: the brief's 7 cases, plus `decodesOneFrameOffAStream`.
  - Channel: the brief's 4 cases, plus `peerCloseEndsTheStream`.
  - PduStream: 4 cases.
  - WasmBinary: 7 cases, including the new `declaredCounts`.
- `PduCodec.lyingBodyIsMalformed` covers more than the brief lists: `MalformedVarint` (6-byte and >32-bit
  varints, and the largest valid one), the status-byte `BadEnum` bound, and the value-count `OverCap`.
- `PduCodec.overCapIsRejectedFromTheHeader` also checks three things: a header exactly at the cap is
  `NeedMoreData`, a length-only header beyond every cap is `OverCap`, and `PduLimits` moves the Load cap.
- `PduCodec.mutatedFramesNeverCrash` mutates 10 000 frames from `mt19937 { 0x5eed }`. It uses `rng() % n`, not
  `std::uniform_int_distribution`, so every standard library produces the same sequence.
  - Every frame that still decodes must have `consumed <= size`, and must re-encode and decode to an equal PDU.
  - The case asserts that at least one frame decoded.
  - The seed is held in a one-element `Seeds` array, as Screen_binaryInput_test does: `bugprone-random-generator-seed`
    flags a literal seed.
- Full suites:
  - `vtextension_test` → All tests passed (10008 assertions in 344 test cases).
  - `vtextension_wasmtime_test` → All tests passed (1004 assertions in 51 test cases).
  - `ctest --preset=clang-asan -R vtextension` → 2/2 passed.
  - `ctest --preset=clang-asan -L lint` → 33/33 passed.
- **Fuzzer.** I built it by hand outside the tree: `clang++ -std=c++23 -O1 -fsanitize=fuzzer,address,undefined
  Codec_fuzz.cpp Codec.cpp`. A 60 s run made 20 049 410 executions with no crash. The CMake option was not
  exercised in a configure.

## Static checks

- clang-tidy, by hand: `clang-tidy -p out/clang-asan --quiet src/vtextension/wasm/remote/proto/Codec.cpp
  src/vtextension/wasm/remote/proto/Codec_test.cpp src/vtextension/wasm/remote/transport/Channel_posix.cpp
  src/vtextension/wasm/remote/transport/PduStream.cpp src/vtextension/wasm/remote/transport/Channel_test.cpp
  src/vtextension/wasm/remote/transport/PduStream_test.cpp src/vtextension/wasm/WasmBinary_test.cpp`. Every
  touched header is covered through `HeaderFilterRegex`.
  - The first run found 3 kinds of finding:
    - `#if defined` → `#ifdef` (readability-use-concise-preprocessor-directives).
    - A moved-from `_buffer` in `Writer::take()`. It now uses `std::exchange`.
    - A constant RNG seed.
  - The second run: no findings. `Channel_win32.cpp` and `Codec_fuzz.cpp` are not in the compile DB.
- Build: `-Werror` clean (clang-asan, `vtextension_test` and `vtextension_wasmtime_test`).
- Windows: `clang-cl --target=x86_64-pc-windows-msvc /winsysroot /opt/msvc /std:c++latest /EHsc /Zs /W4 -Isrc`
  exits 0 with no diagnostics on `Channel_win32.cpp`, `PduStream.cpp` and `Codec.cpp`.
  - To confirm the check bites, I broke a copy of `Channel_win32.cpp` (`CancelIoExx`). It errors as expected.
  - `Channel_test.cpp` could not be syntax-checked there, because the Catch2 headers are not on that include
    path.
  - Nothing ran on Windows.
- Codec_fuzz.cpp: `clang++ -std=c++23 -fsyntax-only -Wall -Wextra` passes, beside the fuzzer run above.
- `/usr/bin/clang-format --dry-run -Werror` and `editorconfig-checker` both pass on every changed file.

## Deviations from the brief, and why

1. **`PduTable` is `inline constexpr std::array<PduRow, 15>` in PduTable.hpp, not `extern std::span<PduRow
   const> const`.**
   - The repo's `.clang-tidy` names a non-constexpr global constant camelBack (ConstantCase), so `PduTable` would
     be a naming error. The phase-1 fix wave hit the same rule.
   - A constexpr array keeps the brief's name and its uses (`.size()`, `[i]`), and puts the row-order
     static_assert in the header.
   - The cost: `decodeBody<Body>` is declared in the header and explicitly instantiated in Codec.cpp, one line
     per alternative. A missing line is a link error.
2. **`Channel::send` returns `std::expected<void, ChannelError>`, not `expected<size_t>`.** It sends every byte
   or fails, so a count would always equal the input size. `receiveSome` returns `expected<size_t>` as briefed.
3. **`Channel::nativeHandle()` was added**, with `NativeHandle = int` on POSIX and `void*` (a HANDLE) on Windows.
   The `endsAreCloseOnExec` test needs it, and phase 3 needs it to hand the helper's end to the child.
4. **`ws2_32` is not linked.** Named pipes are kernel32 only, so the target links nothing on any platform.
5. **`WasmRuntime.hpp` gained `operator==`** on the two description structs. This file is outside the brief's
   list; the round trip of `Loaded` needs it.
6. **Extra tests and sections**, listed under Tests. Every brief-named test exists under its exact name.

## Concerns, and notes for later tasks

- **Windows `shutdown()` does not tell the peer.** On POSIX, `SHUT_RDWR` makes the peer read EOF. On Windows,
  `shutdown()` only cancels and refuses I/O on its own end; the peer sees `ERROR_BROKEN_PIPE` only when the
  handle is closed, which happens when the Channel is destroyed. That is enough for the briefed semantics ("a
  blocked receive returns"). If phase 3 relies on a shutdown waking the other process, it needs
  `DisconnectNamedPipe` on the server end, or a close.
- **D6 "Loaded entry counts ≤ the module's declared counts" is not enforced by the codec**, which cannot see the
  module. `declaredCounts()` exists for RemoteBackend (2.3) to apply that check against the module bytes it sent.
  Until then, `Loaded` is bounded only by 4 × `maxModuleBytes`.
- **`PduLimits` cap arithmetic does not guard against overflow.** `4 × maxModuleBytes` and `+ 5` would wrap only
  for limits near 2^62; the limits are ours, not the peer's.
- **Non-canonical (padded) varints are accepted**, as WasmBinary accepts them. Re-encoding therefore never grows
  a frame, which the mutation and fuzz properties rely on.
- `encodeFrame` does not check element counts, by design (documented). Keeping `argv` within the caps is the
  sender's job, and the reader refuses a frame that exceeds them.
- The receive buffer is zero-filled when it grows (`vector::resize`), at most about one frame cap. Negligible
  next to the I/O.

## Performance, risk, coverage

- **Performance:** none on existing paths. The only change to existing code is header-only equality, plus
  `declaredCounts`, which nothing calls yet. The codec is O(frame).
- **Risk:** low. This is new, unlinked infrastructure; only `vtextension_test` links it. The Windows channel is
  syntax-checked but has not been run.
- **Coverage:** every PDU kind round-trips, and every error path in `decodeFrame` has a test. In the channel,
  the EINTR retry and the non-Linux `fcntl`/`SO_NOSIGPIPE` paths are not exercised on Linux. I did not run a
  coverage build (clang-asan only, per the run rules).

---

## Fix round 1 (review: Needs fixes)

**Commit:** `16cdbe78d fix(vtextension): bound decoded lists, and end the peer's stream on shutdown`

### Changes

1. **(Important) Element caps before decoding.**
   - New protocol constants in `proto/Pdu.hpp`:
     - `MaxImports = 100'000` and `MaxExports = 100'000`: wasmparser's limits, which Wasmtime applies.
     - `MaxSignatureValues = 1'000`: parameters and results, each.
     - `MaxHostFunctions = 1'024`.
   - The existing caps `MaxCallValues` and `MaxHostParameters` keep their roles.
   - In `Codec.cpp`, every vector field is listed in `members()` as `capped(field, Max…)`. These `Capped<Vector>`
     values sit in a `fields(...)` tuple (references for scalars, Capped by value).
   - The vector reader checks the varint count against `maxCount` before it reserves or reads anything, and
     returns `OverCap` if the count is over.
   - The plain `std::vector<T>` read and write overloads are gone, so a list field without a cap no longer
     compiles.
   - `withinCounts()` (the post-decode check) is removed. Its caps are now the per-field caps.
   - Where the caps live: the review suggested table/row metadata. I put the caps on the field, where the field
     list already is, because a PDU can carry several lists with different caps (Loaded has imports, exports and
     signatures). One per-row number could not express that.
   - Worst case now: a Loaded holds at most 100k imports + 100k exports (about 26 MB of structs), plus
     allocation proportional to the body bytes (strings, signature bytes). This is bounded by the 64 MiB body
     cap, with no amplification beyond about 1×.
   - Callable exports are capped at `MaxExports`.
2. **(Important) shutdown() ends the peer's stream on every platform.**
   - The contract text in `Channel.hpp` is updated, at file level and on `shutdown()` / `nativeHandle()`.
   - POSIX is unchanged (`SHUT_RDWR`).
   - Windows: I chose **close-on-drain**, not `DisconnectNamedPipe`. `DisconnectNamedPipe` works only on the
     server end, and the helper holds the client end, so closing the handle is the one mechanism that works for
     both ends.
   - `Native` now holds an `EndState { Open, ShutDown, Closed }` and an `inFlight` counter:
     - Every transfer increments the counter before it reads the state.
     - `shutdown()` moves the state Open→ShutDown with a CAS, then calls `CancelIoEx(pipe, nullptr)`, then
       `closeIfDrained()`.
     - A transfer that finishes decrements the counter and, if the end is not Open, calls `closeIfDrained()`.
     - `closeIfDrained()` closes the handle when `inFlight == 0`, through a CAS ShutDown→Closed, so it closes
       exactly once.
   - Why it is safe: all the atomics are sequentially consistent, so either a transfer sees ShutDown and never
     touches the handle, or shutdown()/closeIfDrained sees it counted and leaves the close to that transfer.
   - The destructor closes the handle only if it is not already Closed. Once closed, `nativeHandle()` returns
     `INVALID_HANDLE_VALUE`.
   - New test `Channel.shutdownEndsThePeersStream`: the peer is blocked in `receiveSome`, the parent calls
     `shutdown()`, the peer gets `Closed`, and the peer's send then gets `Closed`.
3. **(Minor) Fuzz coverage.**
   - `vtextension_remote_fuzz` now compiles `proto/Codec.cpp` itself, with `-fsanitize=fuzzer`, instead of
     linking `vtextension_remote_wire`. The decoder is instrumented, and the library every other target links
     stays uninstrumented.
   - I did not use `-fsanitize=fuzzer-no-link` on the shared library: that would put coverage callbacks into
     `vtextension_test` and every later consumer, which then needs the sanitizer-coverage runtime. This
     deviates from the suggested fix, and the reason is documented in the CMake file.
4. **(Minor) Enum bounds.** `lastEnumerator()` is replaced by `isEnumerator(E)`:
   - For each wire enum, it is a `switch` that names every enumerator and has no `default`. The pedantic build's
     `-Wswitch -Werror` breaks the build when an enumerator is added and not named.
   - `ExtensionError` is checked against `ExtensionErrorTexts.size()` instead, and that table's own static_assert
     keeps it complete.
   - Decoding casts the byte to the enum and asks `isEnumerator`. The enums have a fixed `uint8_t` underlying
     type, so the cast is defined.
5. **(Minor) Owner-only DACL.**
   - `OwnerOnlySecurity::create()` is a static factory that returns a heap object, because the attributes point
     into it.
   - It reads `TokenUser` from the process token and builds an ACL with one `ACCESS_ALLOWED_ACE` (GENERIC_ALL
     for that SID).
   - Both `CreateNamedPipeW` and `CreateFileW` use the resulting `SECURITY_ATTRIBUTES` with
     `bInheritHandle = FALSE`.
   - `vtextension_remote_wire` links `advapi32` on WIN32.
6. **(Minor) New test `Channel.endsAreNotInheritable` (Windows only):** for both ends, `GetHandleInformation` is
   checked to have `HANDLE_FLAG_INHERIT` clear.

New codec test `PduCodec.lyingCountsAreRefusedBeforeReading` covers 9 lists: imports, exports, signature
parameters, signature results, callable exports, host functions, call values, host call arguments and call
results.
- A count of cap + 1 followed by nothing → `OverCap`.
- The same body with count == cap → `Truncated`. This proves the cap check fires first, and that it fires
  before any element allocation.

### Evidence

- **RED, item 1.** With the count check disabled (`if (false && *count > field.maxCount)`), `vtextension_test
  "PduCodec.*"` gave `test cases: 9 | 7 passed | 2 failed`, `assertions: 5362 | 5351 passed | 11 failed`. The
  failures were the 9 caps of the new case and the 2 call-count checks. I restored the check afterwards.
- **RED, item 4.** I added a `Hybrid` enumerator to `MemoryMode`. `clang++ -std=c++23 -fsyntax-only -Wall -Werror
  -Isrc Codec.cpp` then failed with `Codec.cpp:94:17: error: enumeration value 'Hybrid' not handled in switch
  [-Werror,-Wswitch]`. I reverted the change.
- **GREEN.** Covering tests: `vtextension_test "PduCodec.*,Channel.*,PduStream.*,WasmBinary.*"` → All tests passed
  (5461 assertions in 26 test cases).
- **Full suites:**
  - `vtextension_test` → All tests passed (10029 assertions in 346 test cases).
  - `vtextension_wasmtime_test` → All tests passed (1004 assertions in 51 test cases).
  - `ctest --preset=clang-asan -R vtextension` → 100% tests passed, 0 tests failed out of 2.
- **Fuzz option.** I configured a scratch CMake project that `add_subdirectory`s `src/vtextension/wasm/remote`
  with `VTEXTENSION_FUZZING=ON` and clang++, then built `vtextension_remote_fuzz` and ran it 30 s:
  `Done 43135684 runs in 31 second(s)`, `cov: 1008 ft: 2236`. The coverage counters confirm the decoder is
  instrumented.
- **clang-tidy:** `clang-tidy -p out/clang-asan --quiet src/vtextension/wasm/remote/proto/Codec.cpp
  src/vtextension/wasm/remote/proto/Codec_test.cpp src/vtextension/wasm/remote/transport/Channel_posix.cpp
  src/vtextension/wasm/remote/transport/PduStream.cpp src/vtextension/wasm/remote/transport/Channel_test.cpp
  src/vtextension/wasm/remote/transport/PduStream_test.cpp` → no findings.
- **Build:** `-Werror` clean. `/usr/bin/clang-format` applied, and `editorconfig-checker` exits 0.
- **Windows:** `clang-cl --target=x86_64-pc-windows-msvc /winsysroot /opt/msvc /std:c++latest /EHsc /Zs /W4
  -Isrc -I<scratch dir linking /usr/include/catch2>` gives 0 diagnostics on `Channel_win32.cpp`, `Codec.cpp`,
  `Channel_test.cpp` and `Codec_test.cpp`.
  - The Catch2 headers were reached through a scratch directory holding only a `catch2` symlink, so no Linux libc
    headers were on the path.

### Could not be run

- **Windows.** The whole Windows path is syntax-checked only, never executed:
  - the close-on-drain shutdown,
  - the owner-only DACL,
  - `Channel.endsAreNotInheritable`,
  - `Channel.shutdownEndsThePeersStream` on Windows.
- The race reasoning for close-on-drain is argued above, not exercised.
- An elevated admin's process token user is still that user's SID, so the DACL grants that user, not
  Administrators. Phase 5's restricted or AppContainer helper token may need an extra ACE for the sandbox SID.

---

## Fix round 2 (re-review: N1, N2, (a), (b))

**Commit:** `a827bcbbe fix(vtextension): refuse over-cap PDUs at the sender, and cancel only an open pipe`

### Changes

**N1 (Important). Windows `shutdown()` counts itself.**
- `shutdown()` now runs:
  1. `inFlight.fetch_add(1)`
  2. the CAS Open→ShutDown
  3. `CancelIoEx(pipe, nullptr)`, only when it won the CAS
  4. `inFlight.fetch_sub(1)`
  5. `closeIfDrained()`, always
- **Why the increment comes before the CAS, not after it as the review suggested.** With the order "CAS, then
  increment", the review's own race survives in a narrower form:
  - T1 finishes, sees the state not Open, and loads `inFlight == 0`.
  - Only then does S increment.
  - T1's CAS ShutDown→Closed closes the handle, and S's cancel reaches a closed handle.
- **Ordering argument.** Every atomic is sequentially consistent.
  - A closer acts only after it has seen the state not Open. That happens either in a transfer's finish path,
    after its decrement, or in `shutdown()`.
  - S's increment precedes its CAS in the total order. So any closer's `inFlight` load that follows the CAS
    sees S's count, until S's decrement.
  - S's decrement follows its `CancelIoEx`, so no close can precede the cancel.
- **Repeated or concurrent `shutdown()`.** A transfer can finish while some `shutdown()` call holds its transient
  count, see `inFlight != 0`, and leave the close to that call. Every `shutdown()` therefore calls
  `closeIfDrained()` after decrementing. `closeIfDrained()` is a no-op unless the state is ShutDown and nothing is
  in flight, and its CAS ShutDown→Closed makes the close happen exactly once.
- **New transfers after the state change** never touch the handle: they count themselves, read the state, and
  return `Closed`.

**N2 (Minor).** `Channel::nativeHandle()` now documents that it must not be called, nor its value used, while
another thread may call `shutdown()`. On Windows the handle may be closed, and its value reused, from the moment
`shutdown()` begins.

**(a) Fuzz source list.** I kept the fuzzer compiling `proto/Codec.cpp` itself and added a CMake comment saying
why it is not an OBJECT library:
- An OBJECT library is compiled once, with one set of flags.
- The fuzzer's copy needs `-fsanitize=fuzzer`, which the library's copy must not have.
- `Codec.cpp` is the codec's only source; the rest is header-only. A new codec source must join both lists.

**(b) The encoder enforces the element caps.**
- `encodeFrame` now returns `std::expected<std::vector<uint8_t>, EncodeError>`, with
  `enum class EncodeError : uint8_t { OverCap }` declared in Codec.hpp.
- A `withinCaps()` walker visits the same `members()`/`Capped` field lists the reader visits. Every list,
  including nested ones such as an import's signature, must hold at most its `maxCount`.
- `PduStream::send` returns `StreamError::OverCap` for such a PDU and sends nothing, so the stream stays usable.
  The doc comments on `StreamError` and `send` say so.
- Byte caps are not checked at encode time: `encodeFrame` takes no `PduLimits`, as the review allowed.
  `Load`, `MemoryData` and `MemoryWrite` fit their caps whenever the module or memory fits `PduLimits`.
- Callers updated:
  - **Codec tests** use an `encoded()` helper that REQUIREs success.
  - **The "more values than a call carries" decoder section** now builds its over-cap frame by hand, since the
    encoder will no longer produce one.
  - **The fuzz target** traps if a PDU that decoded fails to encode.
  - **The PduStream "other way" test** unwraps the frame.

New tests:
- `PduCodec.encoderRefusesWhatAReaderWould`: 7 cases covering call values, host call arguments, call results,
  imports, a signature nested in an import, callable exports and host functions.
  - One element over the cap → `EncodeError::OverCap`.
  - At the cap → the PDU encodes, then decodes equal.
- `PduStream.overCapIsNotSent`: an over-cap `CreateRuntime` → `StreamError::OverCap`; a following `Release` still
  arrives.
  - It uses a PDU small enough to fit the socket buffer. A first version used 100 001 imports and blocked in
    `send` under the sabotage below, because no one was reading. With the small PDU, a regression fails instead
    of hanging.

### Evidence

- **RED.** With the encoder's cap check disabled, `vtextension_test "PduCodec.encoderRefusesWhatAReaderWould"` gave
  `assertions: 28 | 21 passed | 7 failed`, one failure per case at `Codec_test.cpp:372`. I restored the check
  afterwards.
  - The earlier PduStream variant hung under the same sabotage, and I killed its process by its PID. That is
    what led to the small-PDU rewrite.
- **GREEN, covering tests.** `vtextension_test "PduCodec.*,Channel.*,PduStream.*,WasmBinary.*"` → All tests passed
  (7226 assertions in 28 test cases).
- **Full suites.**
  - `vtextension_test` → All tests passed (11794 assertions in 348 test cases).
  - `ctest --preset=clang-asan -R vtextension` → 100% tests passed, 0 tests failed out of 2.
- **clang-tidy.** `clang-tidy -p out/clang-asan --quiet src/vtextension/wasm/remote/proto/Codec.cpp
  src/vtextension/wasm/remote/proto/Codec_test.cpp src/vtextension/wasm/remote/transport/PduStream.cpp
  src/vtextension/wasm/remote/transport/PduStream_test.cpp src/vtextension/wasm/remote/transport/Channel_test.cpp`
  → no findings.
- **Build:** `-Werror` clean. `clang-format --dry-run -Werror` and `editorconfig-checker` both pass on the changed
  files.
- **Fuzzer.** Rebuilt through the `VTEXTENSION_FUZZING=ON` scratch project and run for 20 s: `Done 29383866 runs in
  21 second(s)`, no crash.
- **Windows.** `clang-cl --target=x86_64-pc-windows-msvc /winsysroot /opt/msvc /std:c++latest /EHsc /Zs /W4 -Isrc
  [-I<catch2-only scratch include>]` → 0 diagnostics on `Channel_win32.cpp`, `Codec.cpp`, `PduStream.cpp`,
  `Codec_test.cpp` and `PduStream_test.cpp`.

### Could not be run

- **Windows behaviour.** The N1 ordering is argued above and syntax-checked, but no Windows test exercises it.
  Neither `Channel.shutdownEndsThePeersStream` nor `Channel.endsAreNotInheritable` has run on Windows.
