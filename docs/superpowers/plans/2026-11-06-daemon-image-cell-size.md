# Daemon Image Cell Size Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Images in daemon-hosted panes are laid out on the cell size of a real client — reported over the native protocol, remembered after the client leaves, and falling back to a configurable 10×20 only before any client was ever seen.

**Architecture:** Clients append their cell pixel size to `ResizeRequest`, `ResizePane` and `ClientHello`. `SessionHost` keeps one daemon-wide cell size — the latest report of a client that displays the resolved grid — and pushes it into every terminal through a new `Terminal::changeCellPixelSize()` seam that updates the PTY's pixel geometry without resizing the grid. The learned value is persisted in the restore snapshot and the live-upgrade image.

**Tech Stack:** C++23, Catch2, CMake presets (`clang-asan`), yaml-cpp config, the vthost native protocol codec.

**Spec:** `docs/superpowers/specs/2026-11-06-daemon-image-cell-size-design.md`

## Global Constraints

- C++23; `.hpp` headers with `#pragma once`; naming per `./.clang-tidy`; `clang-format` C++ only (never `.qml`).
- Zero warnings under `-Werror`; no `NOLINT`, no `#pragma` mutes.
- No `bool` in new API surface where an `enum class` names the decision (AGENT.md "enum class over bool").
- New public functions/members get `///` Doxygen comments.
- Never edit `vendor/core-cpp`. No new third-party dependencies.
- Default cell size: **10×20 pixels** (width 10, height 20).
- Every cell size entering from outside — wire, snapshot, handover image, config — passes `vthost::sanePixelSize` (each extent 1..1024) first; nothing unchecked reaches `Terminal::changeCellPixelSize`, whose precondition aborts.
- Config key: **`daemon.default_cell_pixel_size: { width: 10, height: 20 }`**, in the global `daemon:` section.
- Protocol: fields are **appended** to existing bodies and decoded only when bytes remain; **no** `SupportedVersions` step. Frozen transcripts in `src/vthost/proto/RecordedTranscripts.hpp` must still decode unchanged.
- Only a cell size learned from a client is remembered/persisted; the configured default never is.
- Commits: `Signed-off-by: Christian Parpart <christian@parpart.family>` only, no AI trailers.
- Build: `cmake --build --preset clang-asan`; tests: `ctest --preset=clang-asan`.

## Review Focus

1. **Font zoom that leaves the grid size unchanged** — the new cell size must still reach the daemon and every session's PTY (Task 3 test `SessionHost.cellPixelSize.changes_without_a_grid_change`, Task 6 test `NativeController.sends_a_cell_size_change_with_an_unchanged_grid`).
2. **The only client detaches** — the daemon keeps that client's cell size, not the default (Task 3 test `SessionHost.cellPixelSize.survives_the_client_leaving`).
3. **An older peer / tmux control client** that sends no cell size — the remembered value is untouched (Task 2 decode tests; Task 3 test `SessionHost.cellPixelSize.ignores_clients_without_one`).
4. **A status line is shown** — cell height stays exact across grid resizes (Task 1 test `Terminal.resizeScreenKeepingCellSize.keeps_the_cell_with_a_status_line`).
5. **A hostile or broken cell size off the wire** (0×0, 0×20, 65535×65535) — rejected and logged, nothing applied (Task 3 test `NativeSession.rejects_an_out_of_range_cell_size`).
6. **Two clients at different DPI displaying the same grid** keep re-sending pane sizes after every layout push — the daemon's cell must not flap between them (Task 3 test `SessionHost.cellPixelSize.an_unchanged_re_report_does_not_take_over`).
7. **The first live upgrade from a daemon built before this change** — its image has no `cellPixelSize`, only per-session `pty.pixels`; the successor must keep the real cell, not fall back to 10×20 (Task 4 test `HandoverRestore.migrates_the_cell_size_from_pty_pixels`).

---

### Task 1: Terminal — the cell-size seam, the base-mix fix, kitty's guard, font zoom

**Files:**
- Modify: `src/vtbackend/screen/Terminal.hpp` (near `setCellPixelSize`, ~1525; `resizeScreenKeepingCellSize`, ~1753)
- Modify: `src/vtbackend/screen/Terminal.cpp` (`resizeScreenKeepingCellSize`, ~3600)
- Modify: `src/vtbackend/screen/Screen.cpp` (`renderKittyImage`, ~6870)
- Modify: `src/contour/session/FontControl.cpp` (`resizeToFit`, ~236–246): it returns early when the page size is unchanged, so a zoom that keeps the grid never reaches the PTY. When the page is unchanged but `viewSize / totalPageSize != terminal.cellPixelSize()`, call `terminal.changeCellPixelSize(viewSize / totalPageSize)` under the terminal lock instead of returning. This is also what lets Task 6's sink see a pixel-only change, and fixes local shells' stale `ws_xpixel` on zoom. Test it offscreen in the existing FontControl/session test (find it with `grep -rl resizeToFit src/contour`): a zoom whose new cell still fits the same grid calls the PTY with the new pixels.
- Modify: `src/vtpty/MockPty.hpp` (~100): add `[[nodiscard]] std::optional<ImageSize> pixelSize() const noexcept` returning the private `_pixelSize`, which the tests below and Task 3's need
- Test: `src/vtbackend/screen/Screen_test.cpp`, `src/vtbackend/graphics/KittyGraphics_test.cpp`, `src/vtbackend/graphics/GoodImageProtocol_test.cpp`, `src/vtbackend/graphics/XtSmGraphics_test.cpp`

**Interfaces:**
- Produces: `void vtbackend::Terminal::changeCellPixelSize(ImageSize cell);` — sets the cell size, calls `_pty->resizeScreen(pageSize(), cell * pageSize())` (main page, as `resizeScreen` does), and `reportInBandWindowResize()`. Does not touch the grid, margins, selection or transitions. Precondition `cell.area() > 0` (`Require`). Caller holds the terminal lock. No-op if equal to the current cell size.

- [ ] **Step 1: Write the failing tests**

In `Screen_test.cpp`:
- `Terminal.changeCellPixelSize.reaches_the_pty_without_resizing`: `MockTerm` 10×20 page; set margins with `CSI 3;7 r`; `changeCellPixelSize({Width(9), Height(18)})`; `CHECK(cellPixelSize() == {9,18})`; the MockPty's last resize pixels `== {9*20, 18*10}` (main page); `pageSize()` unchanged; the margin still 3..7 (a `CSI 6 n` after `CSI ?6 h` / check via `currentScreen().margin()`).
- `Terminal.resizeScreenKeepingCellSize.keeps_the_cell_with_a_status_line`: set cell `{10,20}`, show the indicator status line, `resizeScreenKeepingCellSize` to a new size three times; `CHECK(cellPixelSize() == {10,20})` each time.
- Salvaged from #2212 (rewrite each to set a cell size first, and assert layout on it): `Sixel.laid_out_on_the_given_cell` — cell `{10,20}`, ChessBoard (100×100) covers exactly lines 0..4 × columns 0..9 of a 10×20 page; `XTWINOPS.CellArea.reports_the_given_cell` — cell `{8,16}` → `CSI 16 t` replies `\033[6;16;8t`, and `CSI 14 t` replies `\033[4;{16*lines};{8*columns}t` for the same terminal (the two agree).

In `KittyGraphics_test.cpp`:
- `KittyGraphics.laid_out_on_the_given_cell`: cell `{10,20}`, 20×40 RGBA without `c=`/`r=` → fragments exactly at lines 0..1 × columns 0..1.
- `KittyGraphics.unknown_cell_size_is_dropped_unless_c_and_r_are_given`: no cell size; 20×40 without `c=`/`r=` → no cell holds a fragment; the same with `c=2,r=2` → 2×2 cells hold fragments.

In `GoodImageProtocol_test.cpp`: `GoodImageProtocol.Oneshot.laid_out_on_the_given_cell` — cell `{10,20}`, `f=3,w=20,h=40` → 2×2 cells.

In `XtSmGraphics_test.cpp`: `XtSmGraphics.SixelGeometry.Read reports the given cell` — cell `{10,20}`, page 5×11, ceiling 1920×1080 → `\033[?2;0;110;100S`.

- [ ] **Step 2: Run to verify the new-behaviour tests fail**

Run: `cmake --build --preset clang-asan --target vtbackend_test && out/clang-asan/src/vtbackend/vtbackend_test "Terminal.changeCellPixelSize*,Terminal.resizeScreenKeepingCellSize*,KittyGraphics.unknown_cell_size*"`
Expected: compile error for `changeCellPixelSize`; after stubbing, the status-line test fails (cell height shrinks) and the kitty test fails (fragments drawn on a 1×1 clamp).

- [ ] **Step 3: Implement**

- `changeCellPixelSize` as in Interfaces.
- `resizeScreenKeepingCellSize`: pass `cellPixelSize() * clampedTotalPageSize(totalPageSize)` — the total-page basis `resizeScreen` divides by — and correct the comment above it.
- `renderKittyImage`: remove the `std::max(…, 1)` clamps. When `command.columns == 0 || command.rows == 0` and `cellSize.area() == 0`, call `logImageDroppedForUnknownCellSize("kitty", pixels)` and return; the caller's reply path is left as it is.

- [ ] **Step 4: Run the module's tests**

Run: `out/clang-asan/src/vtbackend/vtbackend_test`
Expected: all pass, including the pre-existing `Screen.binaryInput.sixelWithoutCellPixelSize` (still dropped).

- [ ] **Step 5: Commit** — `vtbackend: change the cell size without a resize, and keep it across one`

---

### Task 2: Protocol — cell pixel size on ResizeRequest, ResizePane, ClientHello

**Files:**
- Modify: `src/vthost/proto/Pdu.hpp` (`WireClientArea` ~208, `ClientHello` ~267, `ResizeRequest` ~342, `ResizePane` ~354)
- Modify: `src/vthost/proto/Pdu.cpp` (encode ~382/435/441, decode ~1113/1267/1276)
- Modify: `src/vthost/proto/Negotiation.hpp` (Capability enum + its name table)
- Modify: `src/vthost/testing/PduExamples.hpp`
- Create: `src/vthost/CellPixelBounds.hpp` — `/// @return @p width × @p height as a cell size, or nothing unless each extent is in 1..MaxCellPixelExtent.` `[[nodiscard]] constexpr std::optional<vtpty::ImageSize> sanePixelSize(uint32_t width, uint32_t height) noexcept;` with `inline constexpr uint32_t MaxCellPixelExtent = 1024;`. Qt-free, dependency-free; table-tested in `src/vthost/CellPixelBounds_test.cpp` (0, 1, 1024, 1025 on each axis)
- Test: `src/vthost/proto/Pdu_test.cpp`, `src/vthost/proto/Compatibility_test.cpp`

**Interfaces:**
- Produces:
  - `struct proto::WireCellPixelSize { uint32_t width = 0; uint32_t height = 0; bool operator==(…) const = default; };`
  - `std::optional<WireCellPixelSize> cellPixelSize = std::nullopt;` appended as the **last** member of `ResizeRequest`, `ResizePane` and `ClientHello`.
  - `Capability::CellPixelSize = 20` — informational like `ColorPreference`: a client sends the field regardless, a daemon predating it ignores it; neither end gates on it.

- [ ] **Step 1: Write the failing tests** (in `Pdu_test.cpp`)
  - Round-trip each of the three PDUs with `cellPixelSize = {9, 18}` and with `nullopt`.
  - `ResizeRequest` / `ResizePane` bodies encoded by the old layout (no trailing bytes) decode with `cellPixelSize == nullopt`.
  - A body whose trailing cell size is truncated (only the width varint) is a decode error.
  - Add the `{9,18}` variants to `PduExamples.hpp` so `Compatibility_test` covers them; the frozen v4 transcripts in `src/vthost/testing/RecordedTranscripts.hpp` still decode.

- [ ] **Step 2: Run to verify they fail** — `vthost_test "[proto]"`: compile error on `cellPixelSize`.

- [ ] **Step 3: Implement** — encode the two varints after the existing fields only when the optional is engaged; decode them only when `in.remaining() != 0`, as `LayoutState::servedEdit` does (`Pdu.cpp` ~2020). For `ClientHello`, decoding is the fixed table `ClientHelloFields` (`Pdu.cpp` ~1155, `std::array<…, 7>`): add a `decodeHelloCellPixelSize` row and make it 8; encode it last. Register the capability name in the Capability table; the client offers it in Task 6, the daemon in Task 3 (`src/vthost/Daemon.cpp` ~689).

- [ ] **Step 4: Run** `out/clang-asan/src/vthost/vthost_test "[proto],[negotiation]"` — all pass.

- [ ] **Step 5: Commit** — `vthost: let a client state its cell size in pixels`

---

### Task 3: SessionHost and NativeSession — one daemon-wide cell size

**Files:**
- Modify: `src/vthost/SessionHost.hpp` (`SessionHostConfig` ~831, public API, private state ~1853)
- Modify: `src/vthost/SessionHost.cpp` (`applyClientSize` ~1356, `resizeLocked` ~1520, spawn ~680–740, `unsubscribeStream`)
- Modify: `src/vthost/NativeSession.cpp` (hello ~2278, `ResizeRequest`/`ResizePane` ~1682–1710)
- Modify: `src/vthost/Daemon.cpp` (~689, offered capabilities)
- Test: `src/vthost/SessionHost_test.cpp`, `src/vthost/NativeSession_test.cpp`

**Interfaces:**
- Consumes: `Terminal::changeCellPixelSize(ImageSize)` (Task 1); `proto::WireCellPixelSize`, the three `cellPixelSize` fields, `Capability::CellPixelSize` (Task 2).
- Produces:
  - `SessionHostConfig::defaultCellPixelSize` — `vtpty::ImageSize { vtpty::Width(10), vtpty::Height(20) }`.
  - `void SessionHost::reportCellPixelSize(SessionStreamEvents* client, vtpty::ImageSize cell);` — records the client's cell size. It takes the next `_nextAreaSequence` stamp **only when the value differs from what this client last reported** (or it is the client's first); an unchanged re-report keeps its old stamp. It does **not** apply anything: the `applyClientSize`/`applyPaneSize` that follows settles.
  - `[[nodiscard]] vtpty::ImageSize SessionHost::cellPixelSize() const noexcept;` — the effective size: learned, else `defaultCellPixelSize`.
  - `[[nodiscard]] std::optional<vtpty::ImageSize> SessionHost::learnedCellPixelSize() const noexcept;` — what a client last decided; what Task 4 persists.
  - private: `void settleCellPixelSize();`, `struct ClientCell { vtpty::ImageSize cell; uint64_t sequence = 0; };`, `std::unordered_map<SessionStreamEvents*, ClientCell> _clientCells;`, `std::optional<vtpty::ImageSize> _learnedCellPixelSize;`.

**Resolution rule (`settleCellPixelSize`)**, the one algorithm the signatures do not fix: among `_clientCells` whose client's `_clientAreas` entry equals `_pageSize`, take the highest `sequence`; if none qualifies, the highest `sequence` of all; if `_clientCells` is empty, keep `_learnedCellPixelSize` as is. If the result differs from the current effective size, store it in `_learnedCellPixelSize` and, for every session whose terminal does not already have it, under its terminal lock, `changeCellPixelSize(result)`. No `sessionResized` fan-out (no grid moved). Settling runs at the **end** of `applyClientSize` and `applyPaneSize`, and in `unsubscribeStream` **after** its own `settlePicks(resolveAuthoritativeArea(), …)`, so eligibility reads the re-resolved `_pageSize`.

**One resize per report.** `resizeLocked` passes the host's cell along with the grid — `backing.resizeScreen(size, cellPixelSize() * backing.clampedTotalPageSize(size))` (total-page basis, which `resizeScreen` divides by) — so a report that changes grid and cell moves each PTY once; the settle afterwards finds those terminals already at the new cell and skips them. Because `resizeLocked` must see the new cell before it runs, `applyClientSize`/`applyPaneSize` resolve the cell (pure computation, no terminal touched) before resizing and apply it to the untouched terminals after.

- [ ] **Step 1: Write the failing tests** (`SessionHost_test.cpp`, `MockPty` sessions, `SessionPumping::Manual`)
  - `SessionHost.cellPixelSize.defaults_before_any_client`: no report → every session's terminal `cellPixelSize() == {10,20}` and its MockPty was resized with pixels `{10,20} × page`; `learnedCellPixelSize() == nullopt`.
  - `SessionHost.cellPixelSize.follows_the_latest_client`: client A reports `{9,18}` + area; client B reports `{18,36}` + the same area → sessions use `{18,36}`; A reports again → `{9,18}`.
  - `SessionHost.cellPixelSize.survives_the_client_leaving`: A reports `{9,18}`; `unsubscribeStream(A)` → sessions still `{9,18}`; a session spawned afterwards starts at `{9,18}`.
  - `SessionHost.cellPixelSize.ignores_clients_without_one`: A reports `{9,18}`; client T calls only `applyClientSize` (as tmux does) → still `{9,18}`.
  - `SessionHost.cellPixelSize.changes_without_a_grid_change`: same area, new cell → `changeCellPixelSize` reached every PTY, no `sessionResized` fired.
  - `SessionHost.cellPixelSize.lays_out_a_sixel_after_the_client_left` (the original crash, end to end): A reports `{9,18}`, detaches; write ChessBoard (100×100 sixel) into the session → fragments cover exactly `ceil(100/18)=6` lines × `ceil(100/9)=12` columns. And on a host no client ever touched: the same sixel covers 5×10 (the 10×20 default) and nothing crashes.
  - `SessionHost.cellPixelSize.an_unchanged_re_report_does_not_take_over`: A reports `{9,18}`, B reports `{18,36}` (B wins); A re-sends `{9,18}` with an unchanged area → still `{18,36}`, and no PTY was resized.
  - `SessionHost.cellPixelSize.grid_and_cell_change_resize_once`: one `ResizeRequest` changing both → each MockPty saw exactly one resize, at the new grid and cell.
  - `SessionHost.grid_resize_keeps_the_cell_size`: cell `{9,18}`, `applyClientSize` to a new area → PTY pixels `== {9,18} × new main page`.
  - In `NativeSession_test.cpp`: `NativeSession.applies_the_hello_cell_size_before_the_first_spawn`; `NativeSession.applies_a_resize_cell_size`; `NativeSession.rejects_an_out_of_range_cell_size` — `{0,20}`, `{0,0}`, `{65535,65535}` are logged and not applied (bound: each extent in 1..1024).

- [ ] **Step 2: Run to verify they fail** — `vthost_test "SessionHost.cellPixelSize*,NativeSession.*cell*"`: compile errors, then failures.

- [ ] **Step 3: Implement**
  - Members/API above, settling as the rule says; `unsubscribeStream` erases the client's `_clientCells` entry but never clears `_learnedCellPixelSize`.
  - New session: right after the PTY/terminal is created and before its pump starts (beside `seedRestoredHistory`), `changeCellPixelSize(cellPixelSize())`.
  - `NativeSession`: run every incoming cell through `sanePixelSize` (Task 2); an insane one is logged and dropped, the grid part of the PDU still applies. For hello, `ResizeRequest` and `ResizePane`: `reportCellPixelSize` first, then `applyClientSize`/`applyPaneSize`. The daemon offers `Capability::CellPixelSize` in `Daemon.cpp` ~689.

- [ ] **Step 4: Run** `ctest --preset=clang-asan -L daemon` — all pass.

- [ ] **Step 5: Commit** — `vthost: lay images out on the latest client's cell size`

---

### Task 4: Remember it across restarts and live upgrades

**Files:**
- Modify: `src/vthost/SessionSnapshot.hpp` (`WorkspaceSnapshot`), `src/vthost/SessionSnapshotCodec.cpp` (`encodeSnapshot`, `readBody`), `src/vthost/WorkspaceCapture.cpp` (`captureWorkspace`)
- Modify: `src/vthost/handover/HandoverImage.hpp` (field after `defaultColorPreference`), `src/vthost/handover/HandoverCapture.cpp` (~200), `src/vthost/handover/HandoverRestore.cpp` (~70 and ~220–232)
- Modify: `src/vthost/SessionHost.cpp` (where `config.restore` / `config.handover` are consumed)
- Test: `src/vthost/SessionSnapshotCodec_test.cpp`, `src/vthost/handover/HandoverCodec_test.cpp`, `src/vthost/handover/HandoverRestore_test.cpp`, `src/vthost/SessionHost_test.cpp`

**Interfaces:**
- Consumes: `SessionHost::learnedCellPixelSize()`, `cellPixelSize()` (Task 3); `sanePixelSize` (Task 2). Seeds `_learnedCellPixelSize` directly from the restore paths, which are SessionHost's own construction code.
- Produces: `std::optional<vtpty::ImageSize> WorkspaceSnapshot::cellPixelSize {};` and `std::optional<vtpty::ImageSize> handover::HandoverImage::cellPixelSize {};` (visitor field **13** — 11 is `upgradeGeneration`, 12 is retired and must not be reused; `HandoverImage.hpp` ~166–181).

- [ ] **Step 1: Write the failing tests**
  - Snapshot codec: round-trip with `{9,18}` and with `nullopt`; a file written before this change (no trailing bytes) decodes with `nullopt`.
  - Handover codec: round-trip `cellPixelSize` (extend the existing round-trip at `HandoverCodec_test.cpp:111/148`).
  - `HandoverRestore`: replace the `height > 0` check at `HandoverRestore_test.cpp:481` — a host that learned `{9,18}` with a status line shown hands over; the successor's sessions have exactly `{9,18}` and `learnedCellPixelSize() == {9,18}`. A host that learned nothing hands over `nullopt`; the successor uses its own configured default.
  - `HandoverRestore.migrates_the_cell_size_from_pty_pixels`: an image with `cellPixelSize == nullopt` whose session carries `pty.pixels = {9*80, 18*23}`, `pty.pageSize = 80×23` → successor's `learnedCellPixelSize() == {9,18}` and the session has it.
  - A corrupt `cellPixelSize` (`{0,18}`) in a snapshot or image is ignored (logged), the default stands, nothing aborts.
  - `SessionHost.cellPixelSize.restored_from_a_snapshot`: `config.restore` with `cellPixelSize = {9,18}` → restored sessions use it.

- [ ] **Step 2: Run to verify they fail.**

- [ ] **Step 3: Implement** — snapshot: write `width`,`height` varints after the windows only when engaged; read them only when bytes remain (`readBody` already tolerates none). Capture: `snapshot.cellPixelSize = host.learnedCellPixelSize()`; same into the handover image. Restore: every value read back passes `sanePixelSize`. For a handover, seed the learned cell **before** adopting sessions (where `_defaultColorPreference` is set, `HandoverRestore.cpp` ~70): the image's `cellPixelSize`, else — an image from a daemon built before this change — the first session whose `pty.pixels` and `pty.pageSize` are both non-zero, as `*pty.pixels / pty.pageSize` (the PTY's page is the main page, so this division is exact). Replace the per-session `pty.pixels / totalPageSize()` block (~220–232) with `changeCellPixelSize(cellPixelSize())` under the terminal lock, before `installSession` starts the pump. For a snapshot restore, seed the learned cell before the restored sessions spawn, so the spawn-time apply (Task 3) uses it.

- [ ] **Step 4: Run** `ctest --preset=clang-asan -L daemon` — all pass.

- [ ] **Step 5: Commit** — `vthost: remember the learned cell size across restarts and upgrades`

---

### Task 5: Config — `daemon.default_cell_pixel_size`

**Files:**
- Modify: `src/contour/config/Config.hpp` (`DaemonSettings` ~808), `src/contour/config/Config.cpp` (daemon section loader ~3120), `src/contour/config/ConfigDocumentation.hpp` (~1551 and ~2310–2325)
- Modify: `src/vthost/Daemon.hpp` (~154), `src/vthost/Daemon.cpp` (~1085), `src/contour/cli/ContourApp.cpp` (~1430)
- Modify: `docs/persistent-sessions.md` (config block ~172), `metainfo.xml` (release note)
- Test: `src/contour/config/DaemonSettings_test.cpp`

**Interfaces:**
- Consumes: `SessionHostConfig::defaultCellPixelSize` (Task 3).
- Produces: `vtbackend::ImageSize DaemonSettings::defaultCellPixelSize { Width(10), Height(20) };` and `vtpty::ImageSize DaemonConfig::defaultCellPixelSize` (same default), passed into `SessionHostConfig` at `Daemon.cpp` ~1085.

- [ ] **Step 1: Write the failing tests** (`DaemonSettings_test.cpp`)
  - Absent key → `{10,20}`.
  - `default_cell_pixel_size: { width: 9, height: 18 }` → `{9,18}`.
  - `{ width: 0, height: 18 }`, `{ width: -1, height: 18 }`, `{ width: 9 }`, `"9x18"` → logged as invalid, `{10,20}` kept (follow how the section's other invalid values are reported).

- [ ] **Step 2: Run to verify they fail.**

- [ ] **Step 3: Implement** the field, a new `{width, height}` loader (the existing `ImageSize` loader at `Config.cpp` ~4108 reads `max_width`/`max_height`, so do not reuse it; both extents required, validated with `vthost::sanePixelSize`), the documentation entry next to `size_policy` (one line: "the cell size in pixels images are laid out on before any client has reported one; 10×20 is the VT340's text cell"), the plumbing ContourApp → `DaemonConfig` → `SessionHostConfig`, the `persistent-sessions.md` line, and a `metainfo.xml` entry: "Sixel, kitty and Contour images in daemon-hosted panes are laid out on the attached client's cell size, remembered after it detaches."

- [ ] **Step 4: Run** `out/clang-asan/src/contour/config/contour_config_test "*Daemon*"` (use the target's real name) — pass.

- [ ] **Step 5: Commit** — `contour: add daemon.default_cell_pixel_size`

---

### Task 6: GUI client — report the cell size

**Files:**
- Modify: `src/vthost/client/NativeClient.hpp` (~405–410), `src/vthost/client/NativeClient.cpp` (~549–557)
- Modify: `src/contour/remote/NativeController.hpp` (~712, geometry state), `src/contour/remote/NativeController.cpp` (resize sink ~1587, `reportPaneGeometry` ~1138, `flushGeometry` ~1160–1225, hello ~200–215)
- Test: `src/contour/remote/NativeController_test.cpp`, `src/vthost/client/` client test if one covers `requestResize`

**Interfaces:**
- Consumes: Task 2's fields and `Capability::CellPixelSize`.
- Produces:
  - `void NativeClient::requestResize(uint32_t columns, uint32_t lines, std::optional<proto::WireCellPixelSize> cell);`
  - `void NativeClient::resizePane(uint64_t session, uint32_t columns, uint32_t lines, std::optional<proto::WireCellPixelSize> cell);`
  - `void NativeController::reportPaneGeometry(uint64_t session, uint64_t serial, vtpty::PageSize cells, std::optional<vtpty::ImageSize> pixels);`

- [ ] **Step 1: Write the failing tests** (`NativeController_test.cpp`, with the existing fake client)
  - `NativeController.sends_its_cell_size_with_the_client_area`: a pane resize sink called with cells 80×24 and pixels 720×432 → the `ResizeRequest` sent carries `{9,18}`.
  - `NativeController.sends_a_cell_size_change_with_an_unchanged_grid`: same cells, pixels for `{10,20}` → a report is sent (the grid alone would have suppressed it) carrying `{10,20}`.
  - `NativeController.states_the_last_cell_size_in_the_hello_on_reconnect`: after one report, a reconnect's ClientHello carries it; the first connect's carries `nullopt`.

- [ ] **Step 2: Run to verify they fail.**

- [ ] **Step 3: Implement** — the sink forwards `pixels`; `reportPaneGeometry` derives the cell as `pixels / cells` when both are non-zero and stores it under `_geometryMutex` as `_cellPixelSize` (`std::optional<vtpty::ImageSize>`); `flushGeometry` treats a changed cell like a moved area (sends `ResizeRequest` with the area and re-sends panes) and attaches the cell to every `ResizeRequest`/`ResizePane` it sends; the hello sets `.cellPixelSize` from `_cellPixelSize`; offer `Capability::CellPixelSize`.

- [ ] **Step 4: Run** the `contour_gui_test`/remote test target and `ctest --preset=clang-asan` — all pass.

- [ ] **Step 5: Commit** — `contour: tell the daemon the window's cell size`

---

## Finish

- [ ] Full `cmake --build --preset clang-asan` warning-free; `ctest --preset=clang-asan` green.
- [ ] Manual: `contour daemon`, attach a window, run a sixel demo (e.g. `img2sixel`), detach, run it again headless via `contour client` or the control API, reattach — image at true size, no crash.
- [ ] `/simplify`, then `/code-review`; address all findings.
