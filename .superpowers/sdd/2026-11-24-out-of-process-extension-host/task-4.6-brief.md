### Task 4.6: Single-fetch decoding
**Files:**
- Modify: `vtextension/{CanonicalAbi.hpp,Instance.cpp,ControlImports.cpp,CanonicalAbi_test.cpp}`, every `IGuestMemory` double, `process/testing/LaunchProbe.cpp` (`--mutate`).
- Create: `remote/HostileMemory_test.cpp` (helper_test).

**Produces:**
- `IGuestMemory::read(Address, span<byte> into) -> Result<void>`; `view` is renamed `writable`; `allocate` and `chargeLift` are unchanged.
- `detail::readInteger<U>(memory, address)`, `detail::readBytes<Container>(memory, list, elementSize)`.
- `GuestMemory::read` = one `memcpy` from `IWasmInstance::memory()`.

- [ ] **Step 1: Write the tests.**
  - `"GuestReader.fetchesEachByteOnce"`: a TEMPLATE over every type the host lifts (string, byte list, ConfigMap, LogLevel, each control request).
    - A counting memory stores a sample, then loads it.
    - During loads, `writable()` returns a decoy buffer.
    - Assertions: `load == sample`, and each guest byte is read at most once.
  - `"GuestReader.mutationAfterReadChangesNothing"`: `read()` corrupts its source after copying; the string stays the first copy and the list keeps its first length.
  - `"hostile memory does not break decoding"`:
    - Setup: a 1 MiB object laid out with samples; the probe, launched with `--mutate`, inherits it and flips bytes (`mt19937{0x5eed}`); the test runs 100 000 decodes.
    - Each decode either fails (`Trapped`/`OutOfMemory`) or yields valid UTF-8, in-range enums and lengths ≤ the budget.
    - The probe reports `mutations > 0` and exits 0 at EOF.
- [ ] **Step 2: Build** `vtextension_test vtextension_helper_test extension_host_probe`; expect failure.
- [ ] **Step 3: Interface, Instance and doubles.**
- [ ] **Step 4: Move every decode in `CanonicalAbi.hpp` onto `read`.**
- [ ] **Step 5: Add the probe's `--mutate`.**
- [ ] **Step 6: Run** `vtextension_test "GuestReader.*" "CanonicalAbi.*" "ControlImports.*"` and `helper_test "hostile memory does not break decoding"` → PASS; ctest → PASS.
- [ ] **Step 7: Commit** `fix(vtextension): decode what an extension passes from one copy of its memory`.


### Item 0 — review minors carried from the previous task (do these first, one commit `fix(vtextension): …`)
1. `RemoteBackend.cpp` (~line 619): `largestInitialMemoryPages` is read after `loadInto`; if it fails the call returns `InvalidComponent` but the module stays loaded in the host with no Release. Determine the initial memory size from the module's own bytes before sending Load (or Release on that failure). Test it.
2. `RemoteBackend.cpp` (~lines 640-665): the over-cap / lease checks run after `_slot->live()` and the reload, so after a host death an over-cap module respawns a host and sends a Load before failing `OutOfMemory`. Move the initial-size cap check ahead of any respawn/send. Test: an over-cap module after a host death causes no spawn (launcher count unchanged) and returns `OutOfMemory`.
3. `RemoteBackend.cpp` (~line 692): document at the grant site that a self-grant to `currentProcess()` (in-process host, Windows) leaks one handle when the request fails before the in-process host claims it; production grants go to the child and die with it.

### Controller notes
- D79 (decision text): `abi::IGuestMemory::read(Address, span<byte> into)` (one memcpy) becomes the only way decoders read guest bytes. `view()` is renamed `writable()` (lowering and range proofs only). Enforced by `"GuestReader.fetchesEachByteOnce"`, whose `writable()` hands decoders a decoy buffer.
- Shared guest memory is live since the previous task: `SharedGuestMemory` (remote/) is the parent-side `IGuestMemory`-backing for remote instances; it must implement `read` as one bounded memcpy from the acknowledged size (D77), same as `GuestMemory`.
- The existing hostile "scribble" row (remote tests) passes 199/200 renders today; after this task it should be explained by single-fetch, not luck — note in the report whether its outcome changes.
- `extension_host_probe --mutate` inherits the object like the real host does (the probe's existing launch path); keep it out of shipped binaries.
