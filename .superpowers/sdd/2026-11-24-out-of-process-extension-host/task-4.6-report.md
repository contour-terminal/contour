# Task 4.6 report: single-fetch decoding

Status: **DONE_WITH_CONCERNS** (concerns below are about test strength and the Windows build, not about the change).
Branch `feature/out-of-process-extension-host`, base `80b36a51b`. Nothing pushed.

## Commits

| Hash | Subject |
|---|---|
| `e4ffcfe5f` | fix(vtextension): read a module's memory, and refuse it past the cap, before asking a host (Item 0) |
| `3a49108ff` | fix(vtextension): decode what an extension passes from one copy of its memory |

Both `git commit -s`, no AI trailers, no plan/task references. `ctest --preset=clang-asan -R vtextension` ran
green on each tree before its commit.

## Item 0 (`e4ffcfe5f`)

1. `RemoteRuntime::load` reads `largestInitialMemoryPages` from the module's own bytes **first**, before
   `slotFor`/`live()`/`loadInto`: an unreadable module is `InvalidComponent` with no host started for it and no
   Load sent, so nothing is left loaded without a Release.
   Test `RemoteBackend.unreadableModuleIsNeverLoaded`: a module cut inside its memory section is refused
   `InvalidComponent`; a following good load's Load is the only PDU the scripted host ever received.
2. `RemoteModule::instantiate` checks `_memory.initialBytes > cap` before taking the mutex and before
   `_slot->live()`. Test `RemoteBackend.overCapModuleStartsNoHost`: a one-page runtime, a one-page and a
   two-page module loaded under one identity; the first host answers the first Instantiate for another instance
   (killed); then the over-cap module's instantiate is `OutOfMemory` and `connector->connections()` stays 1.
3. Comment at the `grantHandle` site: production grants are duplicates in the child and die with it; a grant to
   this process (in-process host, Windows) leaks one duplicate per request that fails before the host claims it.

Mutation: with `RemoteBackend.cpp` reverted to HEAD both new tests fail (`2 == 1` connections; Load count `2 == 1`
and the error check). Restored.

**Fallout fixed in the same commit:** `Teardown_test` ("teardown never blocks the owner thread / closing") loaded
16 MiB of zero bytes as a "module"; the parent now refuses that up front, so the load never blocked. It now loads
a real 16 MiB module (header plus one nameless custom section, size as 4-byte LEB128).

## The task (`3a49108ff`)

- `abi::IGuestMemory`: `read(Address, std::span<std::byte> into) -> Result<void>` (one bounded copy; the only way
  lifting reads), `view` renamed `writable` (lowering and range proofs only), `allocate`/`chargeLift` unchanged.
  Header comment states the single-copy rule and why.
- `CanonicalAbi.hpp` `detail`: `decodeInteger<U>(span<byte const, sizeof(U)>)`, `readInteger<U>(memory, address)`
  (replaces `loadInteger`), `readBytes<Container>(memory, list, elementSize)` (byte-sized containers; caller
  charges/bounds first; replaces `viewElements`+`copyBytes`), `checkElements(memory, list, elementSize)` (range
  proof via `writable`, reads nothing). `loadPointerAndLength` is **one 8-byte read** decoded into both words
  (previously a range proof plus two 4-byte reads). Strings: charge, `readBytes`, validate the copy. Byte lists:
  prove, charge, `readBytes`. Other lists: prove, charge, element loads (each element's bytes read once).
  Records/variants/enums/flags unchanged apart from `readInteger`.
- `CanonicalAbi.cpp` `loadByteString`: `readBytes<std::string>`.
- `Instance.cpp` `GuestMemory::read`: `into.size() > UINT32_MAX` → `Trapped`; else one `std::memcpy` from
  `writable()` = `IWasmInstance::memory()`, which for shared memory is `SharedGuestMemory::view`, bounded by the
  acknowledged size (D77).
- Doubles: `FakeGuestMemory` gains `read`, `view` → `writable` (it is the only `IGuestMemory` double in the tree).
  `ControlImports_test`'s `resultUntouched()` now reads through `read`.
- `SharedGuestMemory.hpp`: `view()` doc says what is decoded from it is copied out once.
- `LaunchProbe.cpp`: `claimGrant()` factored out of `takeGrant()`; new `--mutate <helper args> --seed=N`.
- `HostileMemory_test.cpp` (helper_test), added to `vtextension_helper_test`'s sources.
- `ControlImports.cpp`: **not changed** — it only calls codec `load`/`liftFlat` and `checkRange`, which moved with
  the codec. Nothing there reads guest bytes directly.

### Tests

- `GuestReader.fetchesEachByteOnce` — `TEMPLATE_LIST_TEST_CASE` over `std::string`, `ByteList`, `ConfigMap`,
  `LogLevel`, `EventKinds`, `RenderRequest` (everything `Instance.cpp` lifts) and the `Request` of every row of
  `vtcontrol::Methods` (sample = the row's `requestExample` decoded): 32 instantiations. A `FetchCountingMemory`
  stores the sample, `startLoading()`, then loads it: `writable()` returns a 0xA5 decoy while loading; checks
  `load == sample` (by `==`, else by JSON), most-read byte ≤ 1, and that something was read unless the type has
  size 0 (two field-less requests).
- `GuestReader.mutationAfterReadChangesNothing` — `read()` flips every byte after copying it: a string stays the
  first copy; a `ConfigMap` keeps its first length and contents; `loadByteString` output stays the first copy.
- `hostile memory does not break decoding` (helper_test) — `SharedGuestMemory::create(1 MiB, 1 MiB)` laid out to
  the end with rotating samples (string, output byte list via `loadByteString`, ConfigMap, LogLevel, EventKinds,
  a `SplitRequest`), granted to `extension_host_probe --mutate --seed=24301` (0x5eed) with an Instantiate, as
  HandleGrant_test grants; waits for `mutating`; 100 000 decodes, each judged: refused (`Trapped`/`OutOfMemory`,
  and `OutputTooLarge` for the output kind, which is that decoder's documented refusal) or admissible (valid UTF-8,
  lengths within the 64 KiB lift budget / 4 KiB output limit, enums in range, flags within their members, the
  control request's JSON valid UTF-8 and decodable). Then control EOF → `mutations=N` with N > 0 → exit 0.

### Seen failing (mutations, each restored)

| Mutation | Result |
|---|---|
| A: `readInteger` decodes through `writable()` | GuestReader: 17 assertions fail |
| B: `loadPointerAndLength` re-reads the length with a second `readInteger` | GuestReader: 28 fail |
| C: string validated in place via `writable()`, then copied with `readBytes` (classic TOCTOU) | GuestReader: 24 fail. **Hostile decode test: passed 3 of 3 runs** — see concerns |
| Item 0 reverted | both Item 0 tests fail |

### Results

- ASan (`out/clang-asan`), final tree:
  - `ctest --preset=clang-asan -R vtextension`: **5/5 passed** (41.4 s).
  - `vtextension_test`: 442 cases / 25775 assertions, all passed (`GuestReader.*` 32 cases / 188 assertions;
    `GuestReader.*,CanonicalAbi.*,[control]` 79 cases / 1009 assertions).
  - `vtextension_remote_test`: 291 cases / 12090 assertions, all passed.
  - `vtextension_wasmtime_test`: 63 cases (61 passed, 2 skipped).
  - `vtextension_process_test`: 105 cases (104 passed, 1 skipped); `HandleGrant.*` 3 cases after the probe change.
  - `vtextension_helper_test "~[.measure]"`: 158 cases (157 passed, 1 skipped) / 3861 assertions.
  - Hostile decode, ASan: ~1.4–1.9 M mutations per run; 72 791 refused / 27 209 admissible / 0 inadmissible.
- Release (`out/clang-release`, built once, `vtextension_helper_test` only, same lock/nice/-j4):
  `vtextension_helper_test "~[.measure]"` **158 cases / 3874 assertions all passed**. Hostile decode: 636 793
  mutations, 63 119 refused / 36 881 admissible.
- clang-tidy (`-p out/clang-asan`) on every changed `.cpp` plus `ControlImports.cpp`: clean (after the two fixes
  below). `/usr/bin/clang-format` 22 `--dry-run --Werror` and `editorconfig-checker` on every changed file: clean.
- Windows cross-build / wine: **not run** (no `*_win32` file touched). macOS/FreeBSD: not compiled.
- Performance: per lifted integer one virtual `read` + memcpy of ≤ 8 bytes instead of one virtual `view`; a
  (pointer, length) pair is now one call instead of three. Strings/byte lists unchanged in copies (one). Not
  measured.
- Coverage: not measured.

## The hostile "scribble" row

**Unchanged:** `hostile memory through a real host` renders 199/200 (1 failed) in ASan (three runs) and in release.
That row's scribbler flips bytes across the whole memory of the latest instance — the guest's own data and stack
included — so the one failure is the guest failing on its own corrupted state, which no host-side decoding can
change. Single-fetch is proven by the deterministic GuestReader tests, not by that row.

## Deviations

1. **`--seed=N` argument for `--mutate`** instead of a hard-coded `mt19937{0x5eed}` in the probe: clang-tidy's
   `bugprone-random-generator-seed` refuses a constant-seeded engine (a build break under the zero-warning rule,
   NOLINT forbidden). The test passes `0x5eed`, so the sequence is the brief's.
2. **`OutputTooLarge` counts as refused for the output kind** in the hostile test, beside `Trapped`/`OutOfMemory`:
   it is `loadByteString`'s own refusal of a forged length (13 556 decodes were judged inadmissible before this).
3. **No `SharedGuestMemory::read`.** The controller note asks that shared memory be read as one bounded memcpy
   from the acknowledged size; that is what `Instance::GuestMemory::read` does through `IWasmInstance::memory()` →
   `SharedGuestMemory::view` (bounded by the acknowledged size). A separate method would have no production caller.
   The hostile test's `ScribbledMemory` composes the same two steps over a real `SharedGuestMemory`.
4. **The GuestReader template's type list** adds `EventKinds` and `RenderRequest` (also lifted by `Instance.cpp`)
   and covers every catalog request (a superset of the extension-callable ones).
5. **Item 0 test fallout:** `Teardown_test`'s zero-byte "module" replaced by a real padded module (above).
6. `/simplify` and `/code-review` not run (they dispatch agents; the run rules forbid). Self-review: one
   `claimGrant()` shared by `--receive-grant` and `--mutate`; `checkElements` reuses `checkRange`.

## Concerns

1. **The hostile decode test proves robustness, not single-fetch.** Mutation C (validate in place, then copy)
   passed it 3 of 3 times: the window between the two reads is too narrow for random flips across 1 MiB to land
   in it. The decoy-based GuestReader test is what catches double fetches, deterministically.
2. **Windows not built.** `LaunchProbe.cpp` and `HostileMemory_test.cpp` are cross-platform (same grant path as
   HandleGrant_test, `std::jthread`, `mapping->commit` over the whole object), but they were not cross-compiled or
   run under wine.
3. Copying from memory another process writes concurrently is a data race in the C++ model; it is the usual
   shared-memory practice (and the one this design rests on), and TSan was not run on it.
