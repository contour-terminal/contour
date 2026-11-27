### Task 1.6: Sticky interruption by epoch, with no ticker

**Files:**
- Modify: `src/vtextension/wasm/wasmtime/WasmtimeInstance.cpp`, `WasmtimeBackend.cpp`
- Modify: `src/vtextension/wasm/wasmtime/WasmtimeBackend_test.cpp`

**Interfaces:**
- Produces: `IWasmInstance::interrupt()` for Wasmtime. Internal:
  `std::atomic<InterruptState> _interrupt` with `enum class InterruptState : uint8_t { Running, Interrupted }`.

- [ ] **Step 1: Write a Wasmtime-only test.** `TEST_CASE("Wasmtime.idleInstanceNeverWakes")`: run an
  instance with an export that loops 10⁶ iterations, and count epoch-callback invocations through a
  test-only counter on `KindState`. Expected count: 0 when no `interrupt()` happened.
- [ ] **Step 2: Run the interrupt cases; expect failure.** Run:
  `... "WasmContract.interruptStopsLoop" "WasmContract.interruptIsFinal" "Wasmtime.idleInstanceNeverWakes"`.
- [ ] **Step 3: Implement.**
  - Every `call()` checks `_interrupt` first; if `Interrupted`, it fails `Interrupted` without entering
    the guest. Otherwise it sets the store's epoch deadline to 1 tick.
  - `interrupt()` stores `Interrupted`, then calls `wasmtime_engine_increment_epoch` on the runtime's
    engine. It is wait-free: no lock, no allocation.
  - The epoch callback returns a trap when its instance's flag is `Interrupted`. Otherwise it extends the
    deadline by 1 tick and continues. Another instance's interrupt bumps a shared engine epoch, which is
    why the callback must check its own flag.
- [ ] **Step 4: Run the cases.** Expected: PASS.
- [ ] **Step 5: Commit.** `git commit -s -m "feat(vtextension): interrupt Wasmtime calls by epoch, never polling while idle"`

