# Phase 2 — Daemon replication

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A `contour client` pane attached to a daemon holds the same block identity, birth stamps and block records as the session it mirrors — so every later block feature (gutter, scrollbar marks, sticky header, picker, finish notifications) works on an attached pane with no client-side code of its own — and reattaching never replays a finish that happened while detached.

**Architecture:** Rows carry `blockId` and `bornAt` on the wire (`proto::WireLine`), written verbatim into the mirror's lines; the host's session epoch rides `SessionState` so the copied stamps name the right instants. Records travel as `proto::WireCommandBlock`: stated outright in a snapshot's `SessionState` (the newest that fit `SnapshotChunkBytes`), and in a `Delta` whenever they differ from the copy this connection was last sent — revision-gated, oldest first, within a 64 KiB per-delta budget, with ids the host dropped listed as retired. The client accumulates them in `RemoteScreen` with per-record revision stamps, and `ScreenMirror` adopts what changed into the mirror terminal's store: `AdoptMode::Snapshot` on its first replay and for history it never held, `AdoptMode::Live` otherwise, so a finish it watched start raises `commandBlockFinished` exactly once. Head positions are host stable ids and never cross the wire; the mirror derives its own from the head rows it writes. tmux control mode is untouched: its panes are fed raw `%output` bytes into their own terminals and have no OSC 133 passthrough (spec §12).

**Tech Stack:** C++23, the native `vthost` protocol (`src/vthost/proto`), `vtbackend` (`Terminal`, `CommandBlockStore`), Catch2 (`vthost_test`, `vtbackend_test`), CMake presets.

**Spec:** [`docs/drafts/semantic-blocks.md`](../semantic-blocks.md) — read §12 (Daemon mode), and §4.2–§4.5 and §5.4 for what is being replicated. Global constraints: [README](README.md#global-constraints). Protocol background: [`docs/internals/vthost.md`](../../internals/vthost.md) (the codec is unreleased and `CodecVersion` pinned at 1, `src/vthost/proto/Wire.hpp:41`: structs change in place, no version bump).

---

## Design decisions (read before Task 2.1)

These are the choices a reviewer will question; each task cites the one it implements.

- **D1 — What "changed since the connection last saw it" means.** `FollowState` keeps the wire copy last sent per host id (`sentCommandBlocks`) and compares the live record against it field by field (`vthost::matchesWireCommandBlock`, no allocation). The walk runs only when `CommandBlockStore::revision()` moved since the last complete walk — the gate `collectContextState` already uses for OSC 3008. Per-record dirtiness inside the store was rejected: it would need a field or side table the C1 contract does not have, and a revision plus a last-sent copy answers the same question with no change to phase 1. **Cost per flush:** zero while the revision is unchanged; otherwise one walk of at most `maxRecords` records, comparing scalars first and strings (bounded by `MaxRecordedCommandLineBytes` plus the directory) last. **Bytes per flush:** at most `NativeSession::CommandBlockDeltaBytes` (64 KiB) of records, or one record if a single one is larger; the remainder is deferred, the gate left open and the session re-queued, so a flood drains over consecutive debounce windows instead of building a frame the send queue refuses. Records go oldest first, so a client always learns of a record before any newer one.
- **D2 — Snapshots state records in `SessionState`, capped.** Like OSC 3008 contexts, a snapshot states records outright. `SessionState` is one unchunked frame, so it carries the newest records whose estimated size fits `NativeSession::SnapshotChunkBytes` (256 KiB, far below the send-queue bound); any older remainder follows as budgeted increments. A client's table is *replaced* by a snapshot's set.
- **D3 — Head positions do not travel.** `CommandBlockRecord::headStableId/headIdGeneration` name rows in the *host's* stable-id space; the mirror's grid mints its own. `WireCommandBlock` omits them (a deviation from "all record fields", reported in the phase summary). A newly mirrored record starts with `headIdGeneration = vthost::UnknownHeadGeneration` (`UINT64_MAX`, a generation no grid reaches); the mirror sets the real head when it writes the row carrying `LineFlag::Marked` and the record's id. Omitting them also keeps a resize's head rescan, which touches every record, from re-sending every record. **Known limitation:** Task 1.12's guarantee (after any `Terminal::commandBlocks()` call every record's `headIdGeneration` equals the grid's `stableIdGeneration()`) does not hold on a daemon mirror — a record whose head row is not yet written carries `vthost::UnknownHeadGeneration` (until that row arrives, or the next stable-id generation bump rescans it), and consumers treat it as having no head.
- **D4 — Adoption mode on the client.** The mirror's *first* replay adopts every record with `AdoptMode::Snapshot` (that is the attach — nothing it shows is news). After that it adopts with `AdoptMode::Live` unless the record is one it never held *and* older than the newest it holds (history it was never shown: a capped snapshot's remainder, or a record its own smaller bound evicted), which is `Snapshot`. A resync snapshot on a live connection (a resize) therefore still reports a finish whose start the client watched. A host-side `Finished` with a reported end only ever happens to the newest record (OSC 133;D finishes the current one), so the "older → Snapshot" rule can never swallow a real finish. This relies on every attach creating a new ScreenMirror; a reconnect that reuses one must reset it to Prime.
- **D5 — Retirement.** Records the host drops (an empty prompt discarded at the next `133;A`, the bound evicting the oldest, RIS) are listed in `Delta::retiredCommandBlocks`; a snapshot's replacement retires implicitly. The mirror removes them from its store by rebuilding it from the C1 API (`forEachRecord` → `clear` → `adopt(Snapshot)`), at most once per delta and only when a retired id is actually held — at default settings the client's own bound has already evicted what the host's bound evicted, so the rebuild runs for discarded prompts and resets.
- **D6 — The session epoch.** `WireLine::bornAt` is seconds since the *host's* session start. The mirror copies it verbatim and adopts the host's epoch from `SessionState::sessionEpoch`, so `Terminal::lineBirthTime()` answers the same instant on both ends. Translating per line into the client's epoch was rejected: every line older than the client terminal would underflow.

## Interface additions (beyond the README contract)

Phase 2 produces these; later phases may rely on them.

```cpp
// src/vtbackend/screen/Terminal.hpp (namespace vtbackend)
void adoptCommandBlock(CommandBlockRecord record, AdoptMode mode);                 // raises commandBlockFinished
[[nodiscard]] std::chrono::system_clock::time_point sessionEpoch() const noexcept;  // what bornAt counts from
void adoptSessionEpoch(std::chrono::system_clock::time_point epoch) noexcept;      // mirror-only rebinding seam

// src/vthost/proto/Pdu.hpp (namespace vthost::proto)
struct WireLine { /* … */ uint32_t blockId = 0; uint32_t bornAt = 0; /* … */ };
constexpr inline uint8_t CommandBlockHasExitCode, CommandBlockHasCommandStart, CommandBlockHasDuration,
                         CommandBlockPresentMask;
struct WireCommandBlock { /* every CommandBlockRecord field except the head position, wire-typed */ };
struct SessionState { /* … */ int64_t sessionEpoch = 0; std::vector<WireCommandBlock> commandBlocks = {}; };
struct Delta { /* … */ std::vector<WireCommandBlock> commandBlocks = {}; std::vector<uint32_t> retiredCommandBlocks = {}; };
[[nodiscard]] std::size_t estimatedEncodedSize(WireCommandBlock const& block) noexcept;

// src/vthost/TimeWire.hpp, src/vthost/CommandBlockWire.hpp (namespace vthost)
[[nodiscard]] constexpr int64_t toWireTime(std::chrono::system_clock::time_point) noexcept;
[[nodiscard]] constexpr std::chrono::system_clock::time_point fromWireTime(int64_t) noexcept;
[[nodiscard]] constexpr int64_t toWireDuration(std::chrono::steady_clock::duration) noexcept;
[[nodiscard]] constexpr std::chrono::steady_clock::duration fromWireDuration(int64_t) noexcept;
constexpr inline uint64_t UnknownHeadGeneration = std::numeric_limits<uint64_t>::max();
[[nodiscard]] proto::WireCommandBlock toWireCommandBlock(vtbackend::CommandBlockRecord const&);
[[nodiscard]] vtbackend::CommandBlockRecord fromWireCommandBlock(proto::WireCommandBlock const&,
                                                                vtbackend::CommandBlockRecord const* held);
[[nodiscard]] bool matchesWireCommandBlock(vtbackend::CommandBlockRecord const&, proto::WireCommandBlock const&) noexcept;

// src/vthost/client/NativeClient.hpp (namespace vthost::client)
struct MirroredCommandBlock { proto::WireCommandBlock wire; uint64_t revision = 0; };
// RemoteScreen gains: int64_t sessionEpoch; std::map<uint32_t, MirroredCommandBlock> commandBlocks;
//                     uint64_t commandBlocksRevision; uint64_t commandBlocksRetiredAt;
```

**Assumed C1 semantics** (phase 1 owns them; a task's test proves each, and a failure there is a contract
question for the coordinator, not something to work around): `CommandBlockStore::adopt(record, AdoptMode::Live)`
returns a summary exactly when the adopted record is `Finished` with `end == Reported` and the store held no
`Finished` copy of that id (an absent id counts as not finished); `AdoptMode::Snapshot` never returns one;
`Line::reset()` clears `blockId` and `bornAt`; `adoptBlock(0)` and `stampBornAt` on a stamped line are no-ops;
`Terminal::writeToScreen()` stamps birth times like a PTY batch does.

## Build and test commands

| Purpose | Command |
|---|---|
| Build vthost tests | `cmake --build --preset clangcl-debug --target vthost_test` |
| Build vtbackend tests | `cmake --build --preset clangcl-debug --target vtbackend_test` |
| Run by tag | `out/build/clangcl-debug/bin/vthost_test.exe "[blocks]"` |
| Format | `clang-format -i <file>` on every file a task touches, before its commit |

---

### Task 2.1: Wire row — `blockId` and `bornAt` (proto)

**Files:**
- Modify: `src/vthost/proto/Pdu.hpp:345-397` (`WireLine`, after `contextId` at :357)
- Modify: `src/vthost/proto/Pdu.cpp:399-414` (`encodeLine`), `:787-802` (`decodeLine`), `:1065-1074` (`estimatedEncodedSize`)
- Test: `src/vthost/proto/Pdu_test.cpp:90-98` (catalog row), new case appended at end of file

**Interfaces:**
- Consumes: nothing new.
- Produces: `proto::WireLine::blockId` (`uint32_t`, host `CommandBlockId` value, 0 = none), `proto::WireLine::bornAt` (`uint32_t`, seconds since host session start + 1, 0 = unstamped).

- [ ] **Step 1: Write the failing test**

In `src/vthost/proto/Pdu_test.cpp`, give the catalog's row both fields (lines 90-98 become):

```cpp
    auto const line = WireLine {
        .stableId = -3, // signed: SD/unscroll push ids below the origin
        .flags = 0x01FF,
        .contextId = 3,
        .blockId = 0xFFFFFFFF, // the whole 32 bits: host ids are never narrowed
        .bornAt = 0x00FFFFFF,  // vtbackend::Line::MaxBornAt, the saturated stamp
        .columns = 80,
        .cells = { cell },
        .fillForeground = 1,
        .fillBackground = 2,
    };
```

and append at the end of the file:

```cpp
TEST_CASE("a row's command-block id and birth stamp survive the wire", "[vthost][proto][blocks]")
{
    // Unstamped, the smallest stamp, and the extremes: neither field may be narrowed or dropped, and
    // zero must stay zero (it means "no block" / "never stamped", not a value).
    auto const cases = std::vector<std::pair<uint32_t, uint32_t>> {
        { 0, 0 },
        { 1, 1 },
        { std::numeric_limits<uint32_t>::max(), 0x00FFFFFF },
    };
    for (auto const& [blockId, bornAt]: cases)
    {
        auto line = WireLine {};
        line.stableId = 4;
        line.columns = 80;
        line.blockId = blockId;
        line.bornAt = bornAt;
        auto delta = Delta {};
        delta.lines = { line };

        auto const decoded = roundTrip(DecodedPdu { delta });
        auto const* const back = std::get_if<Delta>(&decoded);
        REQUIRE(back != nullptr);
        REQUIRE(back->lines.size() == 1);
        CHECK(back->lines.front().blockId == blockId);
        CHECK(back->lines.front().bornAt == bornAt);
    }
}
```

- [ ] **Step 2: Run it to see it fail**

Run: `cmake --build --preset clangcl-debug --target vthost_test`
Expected: FAIL — does not compile (`'blockId': is not a member of 'vthost::proto::WireLine'`).

- [ ] **Step 3: Add the fields**

In `src/vthost/proto/Pdu.hpp`, directly after `uint16_t contextId = 0;` (line 357):

```cpp
    /// The host's CommandBlockId for this row, or 0 for none.
    ///
    /// Unlike @ref contextId it is NOT translated by a receiver: a mirror's record store holds only
    /// the records the host replicated, under the host's own ids, so the number means the same thing
    /// on both ends. @see WireCommandBlock.
    uint32_t blockId = 0;
    /// When the cursor first reached this row, as seconds since the host's session began, plus one;
    /// 0 = never stamped. 24 bits of meaning (vtbackend::Line::MaxBornAt) in a 32-bit field, counted
    /// from the host's session epoch, which travels in SessionState.
    uint32_t bornAt = 0;
```

- [ ] **Step 4: Encode, decode and estimate them**

In `src/vthost/proto/Pdu.cpp`, `encodeLine` (line 403):

```cpp
        out.u16(line.contextId);
        out.varint(line.blockId);
        out.varint(line.bornAt);
```

`decodeLine` (lines 791-795) becomes:

```cpp
        if (!assign(in.svarint(), line.stableId, error) || !assign(in.u16(), line.flags, error)
            || !assign(in.u16(), line.contextId, error) || !assign(in.varint(), line.blockId, error)
            || !assign(in.varint(), line.bornAt, error) || !assign(in.svarint(), line.promptEndOffset, error)
            || !assign(in.svarint(), line.commandEndOffset, error)
            || !assign(in.varint(), line.columns, error))
            return std::unexpected(error);
```

`estimatedEncodedSize` (lines 1067-1072) becomes:

```cpp
    // encodeCell writes a varint codepoint, a varint extras count, two u8, two u16 and four u32 —
    // 22 fixed bytes plus a 1..4 byte codepoint and whatever clusterExtras adds. encodeLine's own
    // header is two svarints, a u16, four varints (block id and birth stamp among them) and four
    // u32. Both are rounded up to a round number rather than computed exactly: @see the header for
    // why an estimate is the point.
    constexpr auto BytesPerCell = std::size_t { 24 };
    constexpr auto BytesPerRowHeader = std::size_t { 32 };
```

- [ ] **Step 5: Run the tests**

Run: `cmake --build --preset clangcl-debug --target vthost_test` then `out/build/clangcl-debug/bin/vthost_test.exe "[proto]"`
Expected: PASS (the new case, "every catalog PDU round-trips", and "a row's size estimate tracks what the encoder actually writes").

- [ ] **Step 6: Commit**

```bash
clang-format -i src/vthost/proto/Pdu.hpp src/vthost/proto/Pdu.cpp src/vthost/proto/Pdu_test.cpp
git add src/vthost/proto/Pdu.hpp src/vthost/proto/Pdu.cpp src/vthost/proto/Pdu_test.cpp
git commit -F - <<'EOF'
vthost: carry a row's command-block id and birth stamp on the wire

WireLine gains blockId (the host's CommandBlockId, used as-is by a
receiver) and bornAt (seconds since the host's session start, plus one).
The protocol is unreleased, so the struct changes in place.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 2.2: Grid ⇄ wire — fill and apply the new row fields; the parity oracle compares them

**Files:**
- Modify: `src/vthost/GridWire.cpp:4-6` (includes), `:172-205` (`applyWireLine`), `:217-227` (`toWireLine`)
- Modify: `src/vthost/testing/GridParity.cpp:136-162` (`LineFields`)
- Test: `src/vthost/client/ScreenMirror_test.cpp` (add `#include <set>` to the includes at :14-25; append cases at end of file, after :2464)

**Interfaces:**
- Consumes: C1 `Line::blockId()`, `Line::adoptBlock(CommandBlockId)`, `Line::bornAt()`, `Line::stampBornAt(uint32_t)`, `Line::MaxBornAt`; Task 2.1's `WireLine::blockId/bornAt`.
- Produces: `vthost::toWireLine` fills both fields; `vthost::applyWireLine` writes them (block id verbatim, stamp clamped to `Line::MaxBornAt`); `vthost::testing::compareGrids` reports `"blockId"` and `"bornAt"` gaps.

- [ ] **Step 1: Write the failing tests**

Add `#include <set>` to `src/vthost/client/ScreenMirror_test.cpp`'s standard includes, then append:

```cpp
// ---------------------------------------------------------------------------------------------
// Command blocks (semantic blocks, phase 2): row identity and birth stamps.
// ---------------------------------------------------------------------------------------------

TEST_CASE("a wire row's command-block id and birth stamp land on the mirror line", "[vthost][mirror][blocks]")
{
    auto bare = BareMirror { vtbackend::LineCount(10) };
    auto screen = vthost::client::RemoteScreen {};
    screen.columns = 5;
    screen.lines = 1;

    auto seed = proto::Delta {};
    seed.snapshot = 1;
    seed.stableViewportBase = 10;
    seed.stableFloor = 10;
    auto row = rowAt(10, "abcde");
    row.blockId = 42;
    row.bornAt = 7;
    seed.lines.push_back(row);
    screen.apply(seed);
    bare.mirror->apply(screen, seed);

    auto const& line = bare.terminal->primaryScreen().grid().lineAt(vtbackend::LineOffset(0));
    CHECK(line.blockId() == vtbackend::CommandBlockId { 42 }); // the HOST's id, untranslated
    CHECK(line.bornAt() == 7);

    // A stamp wider than the line's 24 bits saturates rather than wrapping into a plausible time.
    auto wide = proto::Delta {};
    wide.stableViewportBase = 10;
    wide.stableFloor = 10;
    auto stamped = rowAt(10, "abcde");
    stamped.bornAt = 0xFFFFFFFF;
    wide.lines.push_back(stamped);
    screen.apply(wide);
    bare.mirror->apply(screen, wide);
    CHECK(bare.terminal->primaryScreen().grid().lineAt(vtbackend::LineOffset(0)).bornAt()
          == vtbackend::Line::MaxBornAt);
}

TEST_CASE("the parity oracle names a block-id and a birth-stamp divergence", "[vthost][parity]")
{
    // Two mirrors fed the same row except for the field under test: the oracle must name exactly
    // that field. Equal grids pass trivially, so this is what proves the two new rows are compared.
    auto const fedWith = [](uint32_t blockId, uint32_t bornAt) {
        auto bare = std::make_unique<BareMirror>(vtbackend::LineCount(10));
        auto screen = vthost::client::RemoteScreen {};
        screen.columns = 5;
        screen.lines = 1;
        auto seed = proto::Delta {};
        seed.snapshot = 1;
        seed.stableViewportBase = 10;
        seed.stableFloor = 10;
        auto row = rowAt(10, "abcde");
        row.blockId = blockId;
        row.bornAt = bornAt;
        seed.lines.push_back(row);
        screen.apply(seed);
        bare->mirror->apply(screen, seed);
        return bare;
    };
    auto const reference = fedWith(7, 61);
    auto const diverging = [&](uint32_t blockId, uint32_t bornAt) {
        auto const other = fedWith(blockId, bornAt);
        auto fields = std::set<std::string> {};
        for (auto const& gap: vthost::testing::compareGrids(*reference->terminal, *other->terminal))
            fields.insert(gap.field);
        return fields;
    };

    CHECK(diverging(7, 61).empty());
    CHECK(diverging(8, 61) == std::set<std::string> { "blockId" });
    CHECK(diverging(7, 62) == std::set<std::string> { "bornAt" });
}

TEST_CASE("PARITY command-block ids and birth stamps", "[vthost][parity]")
{
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    auto const gaps = gapsAfter(&h,
                                session,
                                "\033]133;A\033\\$ \033]133;B\033\\make\r\n"
                                "\033]133;C;cmdline_url=make\033\\built\r\n\033]133;D;0\033\\done",
                                mirrorShows(&h, "done"));
    // The probe engaged: the server stamped the rows this case is about, so two grids equally EMPTY
    // of block ids cannot pass for parity.
    auto const& serverLine = h.serverTerminal(session)->primaryScreen().grid().lineAt(vtbackend::LineOffset(0));
    REQUIRE(serverLine.blockId().value != 0);
    REQUIRE(serverLine.bornAt() != 0);
    checkParity(gaps);
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset clangcl-debug --target vthost_test` then `out/build/clangcl-debug/bin/vthost_test.exe "[blocks],[parity]"`
Expected: FAIL — the first case reads `blockId() == 0` and `bornAt() == 0` (nothing applies them); the oracle case reports no gaps for `diverging(8, 61)` and `diverging(7, 62)`.

- [ ] **Step 3: Fill and apply the fields**

In `src/vthost/GridWire.cpp`, add to the vtbackend includes:

```cpp
#include <vtbackend/shell/CommandBlock.hpp>
```

In `toWireLine`, after `wire.contextId = unbox<uint16_t>(line.contextId());` (line 224):

```cpp
    wire.blockId = line.blockId().value;
    wire.bornAt = line.bornAt();
```

In `applyWireLine`, between the context adoption (lines 189-191) and `if (wire.cells.empty())` (line 192):

```cpp
    // Copied, never translated — the opposite of the context id above, and deliberately so: the
    // mirror's record store holds only what the host replicated, under the host's own ids
    // (@see ScreenMirror::syncCommandBlocks), so there is no second id space to map into. The reset
    // above cleared both fields, and adoptBlock ignores a zero.
    line.adoptBlock(vtbackend::CommandBlockId { wire.blockId });
    // The host's stamp verbatim, counted from the host's session epoch, which the mirror adopts.
    // Clamped rather than trusted: the line keeps 24 bits and the wire field has 32.
    if (wire.bornAt != 0)
        line.stampBornAt(std::min(wire.bornAt, vtbackend::Line::MaxBornAt));
```

- [ ] **Step 4: Compare them in the parity oracle**

In `src/vthost/testing/GridParity.cpp`, append two rows to `LineFields` after the `"fill"` row (before the closing `};` at line 162):

```cpp
        // Compared RAW, unlike the context below: the mirror holds the host's own block ids
        // (@see applyWireLine), so equal numbers ARE the claim.
        LineField { "blockId", [](proto::WireLine const& l) { return std::format("{}", l.blockId); } },
        // Raw too: both ends count from the host's session epoch, which the mirror adopts.
        LineField { "bornAt", [](proto::WireLine const& l) { return std::format("{}", l.bornAt); } },
```

- [ ] **Step 5: Run the tests**

Run: `cmake --build --preset clangcl-debug --target vthost_test` then `out/build/clangcl-debug/bin/vthost_test.exe "[vthost]"`
Expected: PASS — the three new cases and every existing `[parity]` case (`PARITY OSC 133 shell-integration marks` now also compares both fields).

- [ ] **Step 6: Commit**

```bash
clang-format -i src/vthost/GridWire.cpp src/vthost/testing/GridParity.cpp src/vthost/client/ScreenMirror_test.cpp
git add src/vthost/GridWire.cpp src/vthost/testing/GridParity.cpp src/vthost/client/ScreenMirror_test.cpp
git commit -F - <<'EOF'
vthost: mirror rows' block ids and birth stamps, and compare them

toWireLine fills WireLine::blockId/bornAt and applyWireLine writes them
back (the id verbatim, the stamp clamped to the line's 24 bits). The
parity oracle compares both, so a mirror that drops them fails a test.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 2.3: Terminal seams for a mirror — adopt a record with its event; read and adopt the session epoch

**Files:**
- Modify: `src/vtbackend/screen/Terminal.hpp:1381-1385` (after `setContextChain`)
- Modify: `src/vtbackend/screen/Terminal.cpp:3802-3816` (after `Terminal::setContextChain`)
- Create: `src/vtbackend/screen/Terminal_mirrorSeams_test.cpp`
- Modify: `src/vtbackend/CMakeLists.txt` (test list: after `screen/Terminal_input_test.cpp`)

**Interfaces:**
- Consumes: C1 `Terminal::commandBlocks()`, `CommandBlockStore::adopt(CommandBlockRecord, AdoptMode) -> std::optional<CommandBlockSummary>`, `Terminal::Events::commandBlockFinished`, `Terminal::lineBirthTime(Line const&)`, and the wall-clock session epoch phase 1 added for `lineBirthTime()`.
- Produces:
  - `void Terminal::adoptCommandBlock(CommandBlockRecord record, AdoptMode mode);`
  - `[[nodiscard]] std::chrono::system_clock::time_point Terminal::sessionEpoch() const noexcept;`
  - `void Terminal::adoptSessionEpoch(std::chrono::system_clock::time_point epoch) noexcept;`

- [ ] **Step 1: Find phase 1's epoch member**

Run: `grep -n "lineBirthTime" -A15 src/vtbackend/screen/Terminal.cpp`
Expected: `lineBirthTime()` returns `<epoch> + std::chrono::seconds(line.bornAt() - 1)` for a stamped line. That `<epoch>` member (a `std::chrono::system_clock::time_point` set from the injected wall clock at construction) is what the two epoch functions below read and write. The hunks call it `_sessionEpoch`; if phase 1 named it differently, use phase 1's name — never add a second member. If it is declared `const`, drop the `const` and add to its comment: "rebindable only through adoptSessionEpoch(), for a daemon mirror".

- [ ] **Step 2: Write the failing test**

Create `src/vtbackend/screen/Terminal_mirrorSeams_test.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/grid/Line.hpp>
#include <vtbackend/screen/Terminal.hpp>
#include <vtbackend/shell/CommandBlock.hpp>
#include <vtbackend/testing/MockTerm.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <optional>

using namespace std::chrono_literals;
using vtbackend::AdoptMode;
using vtbackend::CommandBlockEnd;
using vtbackend::CommandBlockId;
using vtbackend::CommandBlockRecord;
using vtbackend::CommandBlockState;

namespace
{

/// A record with host id @p id in @p state, built by assignment: CommandBlockRecord has members
/// without default member initializers, so a designated initializer naming a subset would warn.
[[nodiscard]] CommandBlockRecord recordOf(uint32_t id, CommandBlockState state)
{
    auto record = CommandBlockRecord {};
    record.id = CommandBlockId { id };
    record.state = state;
    record.commandLine = "make";
    record.commandLineSource = vtbackend::CommandLineSource::Reported;
    record.promptStartedAt = std::chrono::system_clock::time_point { 1'700'000'000s };
    if (state != CommandBlockState::Prompting)
        record.commandStartedAt = record.promptStartedAt + 2s;
    return record;
}

/// @p id finished, the way OSC 133;D (Reported) or a superseding prompt (Implicit) closes it.
[[nodiscard]] CommandBlockRecord finishedOf(uint32_t id, std::optional<int> exitCode, CommandBlockEnd end)
{
    auto record = recordOf(id, CommandBlockState::Finished);
    record.end = end;
    record.exitCode = exitCode;
    record.duration = 3s;
    return record;
}

} // namespace

TEST_CASE("Terminal.adoptCommandBlock raises a finish only for live, reported news", "[terminal][blocks]")
{
    // MockTerm keeps every finish event the terminal raises (Task 1.8's finishedCommandBlocks).
    auto mock = vtbackend::MockTerm<> { vtbackend::ColumnCount(20), vtbackend::LineCount(4) };
    auto& terminal = mock.terminal;

    SECTION("a record watched running and then finishing raises exactly one event")
    {
        terminal.adoptCommandBlock(recordOf(5, CommandBlockState::Running), AdoptMode::Live);
        CHECK(mock.finishedCommandBlocks.empty());

        terminal.adoptCommandBlock(finishedOf(5, 2, CommandBlockEnd::Reported), AdoptMode::Live);
        REQUIRE(mock.finishedCommandBlocks.size() == 1);
        CHECK(mock.finishedCommandBlocks.front().id == CommandBlockId { 5 });
        CHECK(mock.finishedCommandBlocks.front().exitCode == 2);
        CHECK(mock.finishedCommandBlocks.front().commandLine == "make");

        // Hearing about the same finished record again is not a second finish.
        terminal.adoptCommandBlock(finishedOf(5, 2, CommandBlockEnd::Reported), AdoptMode::Live);
        CHECK(mock.finishedCommandBlocks.size() == 1);
        REQUIRE(terminal.commandBlocks().find(CommandBlockId { 5 }) != nullptr);
        CHECK(terminal.commandBlocks().find(CommandBlockId { 5 })->state == CommandBlockState::Finished);
    }

    SECTION("a command that ran and finished between two adoptions still raises one")
    {
        terminal.adoptCommandBlock(finishedOf(6, 0, CommandBlockEnd::Reported), AdoptMode::Live);
        CHECK(mock.finishedCommandBlocks.size() == 1);
    }

    SECTION("a snapshot adoption is history and never raises")
    {
        terminal.adoptCommandBlock(finishedOf(7, 1, CommandBlockEnd::Reported), AdoptMode::Snapshot);
        CHECK(mock.finishedCommandBlocks.empty());
        CHECK(terminal.commandBlocks().find(CommandBlockId { 7 }) != nullptr);
    }

    SECTION("an implicit close (a nested shell's prompt) never raises")
    {
        terminal.adoptCommandBlock(recordOf(8, CommandBlockState::Running), AdoptMode::Live);
        terminal.adoptCommandBlock(finishedOf(8, std::nullopt, CommandBlockEnd::Implicit), AdoptMode::Live);
        CHECK(mock.finishedCommandBlocks.empty());
    }
}

TEST_CASE("Terminal.adoptSessionEpoch re-anchors line birth times", "[terminal][blocks]")
{
    auto mock = vtbackend::MockTerm<> { vtbackend::ColumnCount(20), vtbackend::LineCount(4) };
    auto const hostEpoch = std::chrono::system_clock::time_point { 1'700'000'000s };

    mock.terminal.adoptSessionEpoch(hostEpoch);
    CHECK(mock.terminal.sessionEpoch() == hostEpoch);

    // A stamp copied from the host means "60 seconds into the HOST's session", stored plus one.
    auto line = vtbackend::Line { vtbackend::ColumnCount(20) };
    line.stampBornAt(61);
    CHECK(mock.terminal.lineBirthTime(line) == hostEpoch + 60s);
}
```

Register it in `src/vtbackend/CMakeLists.txt`, after `screen/Terminal_input_test.cpp`:

```cmake
        screen/Terminal_mirrorSeams_test.cpp
```

- [ ] **Step 3: Run it to see it fail**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test`
Expected: FAIL — does not compile (`'adoptCommandBlock': is not a member of 'vtbackend::Terminal'`).

- [ ] **Step 4: Declare the seams**

In `src/vtbackend/screen/Terminal.hpp`, after `void setContextChain(std::span<ContextId const> ids);` (line 1385):

```cpp
    /// Adopts a command-block record replicated from a daemon host, and raises
    /// Events::commandBlockFinished when the store reports the adoption as a finish — the mirror's
    /// counterpart of OSC 133;D, beside adoptContext().
    ///
    /// Lock-free like adoptContext(): the mirror calls it with the terminal lock already held, so the
    /// event is raised under that lock exactly as the parser thread raises it (@see
    /// Events::commandBlockFinished for what that obliges a listener to do).
    /// @param record The record, under the HOST's id.
    /// @param mode Snapshot for history, which never raises; Live for news.
    void adoptCommandBlock(CommandBlockRecord record, AdoptMode mode);

    /// @return The wall-clock instant this terminal's line birth stamps count from. @see lineBirthTime().
    [[nodiscard]] std::chrono::system_clock::time_point sessionEpoch() const noexcept;

    /// Re-anchors line birth stamps on @p epoch.
    ///
    /// A documented rebinding seam (AGENT.md, configuration at construction) with one caller: a daemon
    /// mirror copies the host's stamps verbatim, and they name the right instants only when counted
    /// from the HOST's epoch. Lock-free, for the reason adoptCommandBlock() gives.
    /// @param epoch The host's session epoch.
    void adoptSessionEpoch(std::chrono::system_clock::time_point epoch) noexcept;
```

- [ ] **Step 5: Define them**

In `src/vtbackend/screen/Terminal.cpp`, after `Terminal::setContextChain` (ends line 3816):

```cpp
void Terminal::adoptCommandBlock(CommandBlockRecord record, AdoptMode mode)
{
    if (auto const summary = commandBlocks().adopt(std::move(record), mode))
        _eventListener.commandBlockFinished(*summary);
}

std::chrono::system_clock::time_point Terminal::sessionEpoch() const noexcept
{
    return _sessionEpoch;
}

void Terminal::adoptSessionEpoch(std::chrono::system_clock::time_point epoch) noexcept
{
    _sessionEpoch = epoch;
}
```

- [ ] **Step 6: Run the tests**

Run: `cmake --build --preset clangcl-debug --target vtbackend_test` then `out/build/clangcl-debug/bin/vtbackend_test.exe "[terminal][blocks]"`
Expected: PASS. A failure in the first case's sections is phase 1's `adopt` disagreeing with the assumed C1 semantics (top of this file): stop and report it to the coordinator rather than adapting the mirror to it.

- [ ] **Step 7: Commit**

```bash
clang-format -i src/vtbackend/screen/Terminal.hpp src/vtbackend/screen/Terminal.cpp src/vtbackend/screen/Terminal_mirrorSeams_test.cpp
git add src/vtbackend/screen/Terminal.hpp src/vtbackend/screen/Terminal.cpp src/vtbackend/screen/Terminal_mirrorSeams_test.cpp src/vtbackend/CMakeLists.txt
git commit -F - <<'EOF'
vtbackend: give a daemon mirror its block and epoch seams

Terminal::adoptCommandBlock adopts a replicated record and raises
commandBlockFinished when the store reports a finish; sessionEpoch and
adoptSessionEpoch let a mirror count copied birth stamps from the
host's session start.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 2.4: The session epoch on the wire

**Files:**
- Create: `src/vthost/TimeWire.hpp`, `src/vthost/TimeWire_test.cpp`
- Modify: `src/vthost/CMakeLists.txt` (library list: after `ImageWire.hpp`; test list: after `ImageWire_test.cpp`)
- Modify: `src/vthost/proto/Pdu.hpp:323-325` (`SessionState`, after `contextChain`)
- Modify: `src/vthost/proto/Pdu.cpp:378-380` (encode `SessionState`), `:758-762` (decode `SessionState`)
- Modify: `src/vthost/NativeSession.cpp:25-33` (includes), `:545-546` (`collectLiveState` snapshot block)
- Modify: `src/vthost/client/NativeClient.hpp:88-90` (`RemoteScreen`), `src/vthost/client/NativeClient.cpp:58-62` (`RemoteScreen::apply(SessionState)`)
- Modify: `src/vthost/client/ScreenMirror.cpp:20-27` (includes), `:472-477` (end of `applySessionState`)
- Test: `src/vthost/proto/Pdu_test.cpp:128-148` (catalog `SessionState`), `:465-466` (`sessionStateWith`); `src/vthost/client/ScreenMirror_test.cpp` (append)

**Interfaces:**
- Consumes: Task 2.3's `Terminal::sessionEpoch()`, `Terminal::adoptSessionEpoch()`.
- Produces: `vthost::toWireTime`, `fromWireTime`, `toWireDuration`, `fromWireDuration` (nanoseconds as `int64_t`); `proto::SessionState::sessionEpoch` (`int64_t`, 0 = not stated); `client::RemoteScreen::sessionEpoch`.

- [ ] **Step 1: Write the failing tests**

Create `src/vthost/TimeWire_test.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <chrono>

#include <vthost/TimeWire.hpp>

using namespace std::chrono_literals;

TEST_CASE("a wall-clock instant survives the wire's nanoseconds", "[vthost][wire][blocks]")
{
    // Divisible by a microsecond, the coarsest system_clock period in use (libc++), so the value is
    // exact on every platform's clock rather than merely close.
    auto const instant = std::chrono::system_clock::time_point {
        std::chrono::duration_cast<std::chrono::system_clock::duration>(1'700'000'000'123'456'000ns)
    };
    CHECK(vthost::toWireTime(instant) == 1'700'000'000'123'456'000);
    CHECK(vthost::fromWireTime(vthost::toWireTime(instant)) == instant);
    CHECK(vthost::toWireTime(std::chrono::system_clock::time_point {}) == 0);
}

TEST_CASE("a steady-clock span survives the wire's nanoseconds", "[vthost][wire][blocks]")
{
    auto const span = std::chrono::duration_cast<std::chrono::steady_clock::duration>(12'345'000ns);
    CHECK(vthost::toWireDuration(span) == 12'345'000);
    CHECK(vthost::fromWireDuration(vthost::toWireDuration(span)) == span);
}
```

Register it in `src/vthost/CMakeLists.txt`, in the `vthost_test` list after `ImageWire_test.cpp`:

```cmake
        TimeWire_test.cpp
```

In `src/vthost/proto/Pdu_test.cpp`, the catalog's `SessionState` (line 148) now ends:

```cpp
                           .contexts = { context },
                           .contextChain = { 1, 3 },
                           .sessionEpoch = 1'700'000'000'123'456'000 },
```

and `sessionStateWith` gains, after `body.varint(0); // contextChain` (line 466):

```cpp
        body.svarint(0); // sessionEpoch
```

Append to `src/vthost/client/ScreenMirror_test.cpp`:

```cpp
namespace
{
/// Waits until @p settle holds, then detaches, so `drive` returns with the mirror caught up.
Task<void> settleThenDetach(MirrorHarness* h, std::function<bool()> settle)
{
    co_await waitUntil(&h->loop, std::move(settle));
    h->client->detach();
}
} // namespace

TEST_CASE("the mirror counts birth stamps from the host's session epoch", "[vthost][mirror][blocks]")
{
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    auto* const server = h.serverTerminal(session);
    // An hour apart, so a mirror counting from its OWN epoch is visibly wrong rather than off by the
    // milliseconds between the two terminals' construction.
    server->adoptSessionEpoch(server->sessionEpoch() - std::chrono::hours(1));
    server->writeToScreen("stamped");

    h.loop.blockOn(drive(&h, settleThenDetach(&h, mirrorShows(&h, "stamped"))));

    auto const& serverLine = server->primaryScreen().grid().lineAt(vtbackend::LineOffset(0));
    auto const& mirrorLine = h.mirror->primaryScreen().grid().lineAt(vtbackend::LineOffset(0));
    REQUIRE(serverLine.bornAt() != 0); // the probe engaged: the host stamped the row
    CHECK(h.mirror->sessionEpoch() == server->sessionEpoch());
    CHECK(mirrorLine.bornAt() == serverLine.bornAt());
    CHECK(h.mirror->lineBirthTime(mirrorLine) == server->lineBirthTime(serverLine));
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset clangcl-debug --target vthost_test`
Expected: FAIL — does not compile (`vthost/TimeWire.hpp` not found; `'sessionEpoch': is not a member of 'vthost::proto::SessionState'`).

- [ ] **Step 3: Create the time encoding**

Create `src/vthost/TimeWire.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Instants and spans as the native protocol spells them: signed 64-bit NANOSECONDS — wall-clock
/// instants since the Unix epoch, steady-clock spans as plain lengths.
///
/// One unit for every clock and platform, because the two ends need not share one: system_clock ticks
/// in 100 ns on MSVC, 1 µs in libc++ and 1 ns in libstdc++. Nanoseconds are at least as fine as all
/// three, so an instant crossing between two builds on one platform is exact, and int64_t nanoseconds
/// reach the year 2262.

#include <chrono>
#include <cstdint>

namespace vthost
{

/// @param when A wall-clock instant.
/// @return It as the wire spells it: nanoseconds since the Unix epoch.
[[nodiscard]] constexpr int64_t toWireTime(std::chrono::system_clock::time_point when) noexcept
{
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(when.time_since_epoch()).count());
}

/// The inverse of @ref toWireTime, truncated to this platform's system_clock period.
/// @param nanoseconds Nanoseconds since the Unix epoch.
/// @return The instant they name.
[[nodiscard]] constexpr std::chrono::system_clock::time_point fromWireTime(int64_t nanoseconds) noexcept
{
    return std::chrono::system_clock::time_point {
        std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::nanoseconds { nanoseconds })
    };
}

/// @param span A steady-clock length (a command's duration).
/// @return It in nanoseconds.
[[nodiscard]] constexpr int64_t toWireDuration(std::chrono::steady_clock::duration span) noexcept
{
    return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(span).count());
}

/// The inverse of @ref toWireDuration.
/// @param nanoseconds A length in nanoseconds.
/// @return It as a steady-clock duration.
[[nodiscard]] constexpr std::chrono::steady_clock::duration fromWireDuration(int64_t nanoseconds) noexcept
{
    return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::nanoseconds { nanoseconds });
}

} // namespace vthost
```

Add `TimeWire.hpp` to the `vthost` library list in `src/vthost/CMakeLists.txt`, after `ImageWire.hpp`.

- [ ] **Step 4: Carry the epoch in SessionState**

In `src/vthost/proto/Pdu.hpp`, after `std::vector<uint16_t> contextChain;` (line 324):

```cpp
    /// The host terminal's session epoch — the wall-clock instant every WireLine::bornAt counts
    /// from — as nanoseconds since the Unix epoch (vthost/TimeWire.hpp); 0 = not stated. A mirror
    /// adopts it, or the stamps it copied would name instants shifted by however much later the
    /// client terminal was created than the host's.
    int64_t sessionEpoch = 0;
```

In `src/vthost/proto/Pdu.cpp`, `encodeBody(Writer&, SessionState const&)` gains after the `contextChain` loop (line 380):

```cpp
        out.svarint(pdu.sessionEpoch);
```

and `decodeSessionState` gains between the `contextChain` decode (ends line 761) and `return pdu;`:

```cpp
        if (!assign(in.svarint(), pdu.sessionEpoch, error))
            return std::unexpected(error);
```

- [ ] **Step 5: Send it, keep it, adopt it**

`src/vthost/NativeSession.cpp`: add `#include <vthost/TimeWire.hpp>` after `#include <vthost/PduPump.hpp>` (keeping the block sorted), and in `collectLiveState`'s snapshot block, after the `contextChain` loop (line 546):

```cpp
        snap.sessionEpoch = toWireTime(terminal.sessionEpoch());
```

`src/vthost/client/NativeClient.hpp`, `RemoteScreen`, after `contextChain` (line 90):

```cpp
    /// The host's session epoch (nanoseconds since the Unix epoch) every row's bornAt counts from;
    /// 0 until a SessionState stated it. @see proto::SessionState::sessionEpoch.
    int64_t sessionEpoch = 0;
```

`src/vthost/client/NativeClient.cpp`, `RemoteScreen::apply(SessionState const&)`, after `contextChain = state.contextChain;` (line 62):

```cpp
    sessionEpoch = state.sessionEpoch;
```

`src/vthost/client/ScreenMirror.cpp`: add `#include <vthost/TimeWire.hpp>` after `#include <vthost/StatusWire.hpp>`, and at the end of `applySessionState` (after the progress block, line 477):

```cpp
    // The rows carry their birth stamps as seconds since the HOST's session began, so they name the
    // right instants only when counted from the host's epoch. Zero is "never stated" (a bare test
    // screen), not 1970. Compared first, like everything above: a replay runs on every resize.
    if (screen.sessionEpoch != 0)
        if (auto const epoch = fromWireTime(screen.sessionEpoch); _terminal->sessionEpoch() != epoch)
            _terminal->adoptSessionEpoch(epoch);
```

- [ ] **Step 6: Run the tests**

Run: `cmake --build --preset clangcl-debug --target vthost_test` then `out/build/clangcl-debug/bin/vthost_test.exe "[proto],[wire],[blocks]"`
Expected: PASS.

- [ ] **Step 7: Commit**

```bash
clang-format -i src/vthost/TimeWire.hpp src/vthost/TimeWire_test.cpp src/vthost/proto/Pdu.hpp src/vthost/proto/Pdu.cpp src/vthost/proto/Pdu_test.cpp src/vthost/NativeSession.cpp src/vthost/client/NativeClient.hpp src/vthost/client/NativeClient.cpp src/vthost/client/ScreenMirror.cpp src/vthost/client/ScreenMirror_test.cpp
git add src/vthost/TimeWire.hpp src/vthost/TimeWire_test.cpp src/vthost/CMakeLists.txt src/vthost/proto/Pdu.hpp src/vthost/proto/Pdu.cpp src/vthost/proto/Pdu_test.cpp src/vthost/NativeSession.cpp src/vthost/client/NativeClient.hpp src/vthost/client/NativeClient.cpp src/vthost/client/ScreenMirror.cpp src/vthost/client/ScreenMirror_test.cpp
git commit -F - <<'EOF'
vthost: replicate the session epoch birth stamps count from

SessionState states the host terminal's session epoch and the mirror
adopts it, so a copied birth stamp names the same instant on both ends.
TimeWire.hpp fixes the wire's unit for instants and spans: nanoseconds.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 2.5: Wire record — `WireCommandBlock` in `SessionState` and `Delta` (proto)

Implements D2, D3 and D5's wire half.

**Files:**
- Modify: `src/vthost/proto/Pdu.hpp:282-283` (new constants + `WireCommandBlock` after `WireContext`), `SessionState` (after `sessionEpoch`, Task 2.4), `Delta:528-552` (fields after `contexts`, `hasChanges`), `:778` (estimate overload after the `WireLine` one)
- Modify: `src/vthost/proto/Pdu.cpp:349` (table + encoder after `encodeContext`), `SessionState` encode/decode (after `sessionEpoch`), `Delta` encode `:487-489` / decode `:879-881`, `:726` (decoders after `decodeContext`), `:1074` (estimate after the `WireLine` one)
- Modify: `src/vthost/proto/PduTrace.cpp:90-133` (counts only)
- Test: `src/vthost/proto/Pdu_test.cpp` (helper after `deltaHeaderBody` :46; catalog :128-189; `sessionStateWith` after the `sessionEpoch` line; new cases at end), `src/vthost/proto/PduTrace_test.cpp:24-44, :129-136, :189-206`

**Interfaces:**
- Consumes: nothing new.
- Produces (namespace `vthost::proto`):
  - `constexpr inline uint8_t CommandBlockHasExitCode = 1, CommandBlockHasCommandStart = 2, CommandBlockHasDuration = 4, CommandBlockPresentMask = 0x07;`
  - `struct WireCommandBlock { uint32_t id; uint8_t state, end, commandLineSource, locality, present; std::string commandLine, workingDirectory; uint8_t outcomeExit, outcomeSignal; uint64_t outcomeStatus; int64_t promptStartedAt, exitCode, commandStartedAt, duration; };`
  - `SessionState::commandBlocks`, `Delta::commandBlocks`, `Delta::retiredCommandBlocks` (`std::vector<uint32_t>`).
  - `[[nodiscard]] std::size_t estimatedEncodedSize(WireCommandBlock const& block) noexcept;` — never below the encoded size.

- [ ] **Step 1: Write the failing tests**

In `src/vthost/proto/Pdu_test.cpp`, add to the anonymous namespace after `deltaHeaderBody()`:

```cpp
/// A command-block record with every optional field present and every scalar off its default, so a
/// round trip proves each one is written AND read back.
WireCommandBlock sampleBlock()
{
    auto block = WireCommandBlock {};
    block.id = 0xFFFFFFF0;
    block.state = 2;             // Finished
    block.end = 1;               // Implicit
    block.commandLineSource = 1; // Reported
    block.locality = 2;          // Foreign
    block.present = CommandBlockPresentMask;
    block.commandLine = "make -j8 test";
    block.workingDirectory = "/home/user/src/contour";
    block.outcomeExit = 3;  // Crash
    block.outcomeSignal = 11;
    block.outcomeStatus = 139;
    block.promptStartedAt = 1'700'000'000'123'456'000;
    block.exitCode = -1; // signed on the wire: the receiver judges the range, not the codec
    block.commandStartedAt = 1'700'000'002'000'000'000;
    block.duration = 3'000'000'000;
    return block;
}
```

In the catalog, `SessionState` ends `.sessionEpoch = 1'700'000'000'123'456'000, .commandBlocks = { sampleBlock() } },` and `Delta` ends:

```cpp
                    .contexts = { context },
                    .commandBlocks = { sampleBlock() },
                    .retiredCommandBlocks = { 3, 0xFFFFFFFF } },
```

`sessionStateWith` gains after `body.svarint(0); // sessionEpoch`:

```cpp
        body.varint(0); // commandBlocks
```

Append:

```cpp
TEST_CASE("a command-block record's optional fields travel only when present", "[vthost][proto][blocks]")
{
    auto const sizeWith = [](WireCommandBlock const& block) {
        auto delta = Delta {};
        delta.commandBlocks = { block };
        auto sink = Writer {};
        encodePdu(sink, 1, DecodedPdu { delta });
        return sink.size();
    };

    SECTION("an absent field costs nothing on the wire")
    {
        // All three optionals zero either way; only the presence bits differ, and each present one
        // costs exactly its one-byte svarint(0).
        auto absent = sampleBlock();
        absent.present = 0;
        absent.exitCode = 0;
        absent.commandStartedAt = 0;
        absent.duration = 0;
        auto present = absent;
        present.present = CommandBlockPresentMask;
        CHECK(sizeWith(present) == sizeWith(absent) + 3);
        // Built by assignment: Delta has members without default initializers, so a designated
        // initializer naming one field would trip -Wmissing-designated-field-initializers.
        auto delta = Delta {};
        delta.commandBlocks = { absent };
        CHECK(roundTrip(DecodedPdu { delta }) == DecodedPdu { delta });
    }

    SECTION("an unknown presence bit is cleared and the stream stays in step")
    {
        auto future = sampleBlock();
        future.present = 0xFF; // bits a later build might assign, which this one reads past
        auto delta = Delta {};
        delta.commandBlocks = { future, sampleBlock() };

        auto const decoded = roundTrip(DecodedPdu { delta });
        auto const* const back = std::get_if<Delta>(&decoded);
        REQUIRE(back != nullptr);
        REQUIRE(back->commandBlocks.size() == 2);
        CHECK(back->commandBlocks[0].present == CommandBlockPresentMask);
        CHECK(back->commandBlocks[1] == sampleBlock()); // the record after it decoded intact
    }
}

TEST_CASE("Delta::hasChanges answers for command-block records and retirements", "[vthost][proto][blocks]")
{
    auto withBlock = Delta {};
    withBlock.commandBlocks = { WireCommandBlock {} };
    CHECK(withBlock.hasChanges());

    auto withRetired = Delta {};
    withRetired.retiredCommandBlocks = { 7 };
    CHECK(withRetired.hasChanges());
}

TEST_CASE("a command-block record's size estimate never under-counts", "[vthost][proto][blocks]")
{
    // The estimate budgets a delta's records (NativeSession::CommandBlockDeltaBytes) and caps a
    // snapshot's; an under-count would let a "bounded" frame exceed what it was sized against.
    auto const encodedSize = [](std::vector<WireCommandBlock> blocks) {
        auto delta = Delta {};
        delta.commandBlocks = std::move(blocks);
        auto sink = Writer {};
        encodePdu(sink, 1, DecodedPdu { delta });
        return sink.size();
    };
    auto const empty = encodedSize({});
    for (auto const length: { std::size_t { 0 }, std::size_t { 40 }, std::size_t { 4096 } })
    {
        auto block = sampleBlock();
        block.commandLine = std::string(length, 'x');
        auto const actual = encodedSize({ block }) - empty;
        auto const estimate = estimatedEncodedSize(block);
        CHECK(estimate >= actual);
        CHECK(actual * 2 >= estimate); // and not so generous that a budget fills with air
    }
}
```

In `src/vthost/proto/PduTrace_test.cpp`, `sampleSessionState()` gains before `return value;`:

```cpp
    auto block = WireCommandBlock {};
    block.commandLine = "secret-command";
    block.workingDirectory = "/home/user/secret-dir";
    value.commandBlocks = { block };
```

`sampleDelta()` gains before `return value;`:

```cpp
    auto block = WireCommandBlock {};
    block.commandLine = "secret-command";
    value.commandBlocks = { block };
    value.retiredCommandBlocks = { 3 };
```

the `"Delta reports its cursor and its batch sizes"` section gains:

```cpp
        CHECK(text.contains("blocks=1"));
        CHECK(text.contains("retired=1"));
```

and the loop in `"the PDU trace never reveals payload contents"` gains:

```cpp
        CHECK_FALSE(text.contains("secret-command")); // a command line a shell reported
        CHECK_FALSE(text.contains("secret-dir"));     // the directory it ran in
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset clangcl-debug --target vthost_test`
Expected: FAIL — does not compile (`'WireCommandBlock': undeclared identifier`).

- [ ] **Step 3: Declare the record and the new fields**

In `src/vthost/proto/Pdu.hpp`, after `WireContext`'s closing `};` (line 282):

```cpp
/// The optional fields a command-block record's `present` byte may name, as the WIRE numbers them.
///
/// Proto-owned and frozen for the reason ContextPresentMask is: they travel verbatim. A bit this build
/// does not assign is cleared on decode, so the reads that follow stay in step with the sender.
constexpr inline uint8_t CommandBlockHasExitCode = 1U << 0;     ///< `exitCode` travelled.
constexpr inline uint8_t CommandBlockHasCommandStart = 1U << 1; ///< `commandStartedAt` travelled.
constexpr inline uint8_t CommandBlockHasDuration = 1U << 2;     ///< `duration` travelled.
constexpr inline uint8_t CommandBlockPresentMask = 0x07;        ///< Every bit this build assigns.

/// One shell command block's record (vtbackend::CommandBlockRecord), as the host holds it.
///
/// A row names its block by WireLine::blockId; the record travels beside the rows — stated outright
/// in a snapshot's SessionState, and in a Delta whenever it changed since this connection last saw
/// it. The id is the HOST's, and a receiver keeps it as-is.
///
/// Deliberately ABSENT: the record's cached head position (`headStableId`, `headIdGeneration`). Those
/// are stable row ids in the HOST's grid, and a mirror's grid mints its own — carried across they
/// would name the wrong row, or, once the two generations happened to coincide, a plausible wrong
/// one. The mirror derives its own from the head rows it writes. Leaving them out also keeps a
/// resize's head rescan, which touches every record, from re-sending every record.
///
/// Enum-ish bytes travel raw and are validated by the consumer (@see vthost/CommandBlockWire.hpp),
/// which is what keeps this codec free of vtbackend.
struct WireCommandBlock
{
    uint32_t id = 0;               ///< The host's block id.
    uint8_t state = 0;             ///< vtbackend::CommandBlockState.
    uint8_t end = 0;               ///< vtbackend::CommandBlockEnd.
    uint8_t commandLineSource = 0; ///< vtbackend::CommandLineSource.
    uint8_t locality = 0;          ///< vtbackend::ContextLocality of `workingDirectory`.
    uint8_t present = 0;           ///< CommandBlockHas* bits.
    std::string commandLine = {};      ///< RAW, as the host recorded it; sanitised per use, never here.
    std::string workingDirectory = {}; ///< The directory snapshot taken at OSC 133;C.
    uint8_t outcomeExit = 0;           ///< vtbackend::ContextExit.
    uint8_t outcomeSignal = 0;         ///< vtbackend::ContextSignal, as a Linux signal number.
    uint64_t outcomeStatus = 0;        ///< `vtbackend::ContextOutcome::status`, verbatim (a varint on the wire).
    int64_t promptStartedAt = 0; ///< Wall clock, nanoseconds since the Unix epoch (vthost/TimeWire.hpp).
    /// The three optionals: meaningful only with their CommandBlockHas* bit, zero otherwise. All
    /// signed 64-bit so one table drives their encoding; an exit code outside `int` is the
    /// receiver's to reject.
    int64_t exitCode = 0;
    int64_t commandStartedAt = 0; ///< Wall clock, nanoseconds since the Unix epoch.
    int64_t duration = 0;         ///< Steady-clock nanoseconds from OSC 133;C to ;D.
    bool operator==(WireCommandBlock const&) const = default;
};
```

In `SessionState`, after `int64_t sessionEpoch = 0;`:

```cpp
    /// The command-block records, oldest first, stated outright: the newest of them whose estimated
    /// size fits NativeSession::SnapshotChunkBytes. Any older remainder follows as increments. A
    /// receiver REPLACES its table with this set.
    std::vector<WireCommandBlock> commandBlocks = {};
```

In `Delta`, after `std::vector<WireContext> contexts;` (line 532):

```cpp
    /// Command-block records that changed since this connection last saw them, oldest first, within
    /// a per-delta byte budget (the rest follow in the next delta). Empty on a snapshot, whose
    /// records travel in SessionState. @see NativeSession::collectCommandBlockState.
    std::vector<WireCommandBlock> commandBlocks = {};
    /// Host ids this connection was told about that the host no longer holds — a prompt discarded by
    /// the next one, the oldest evicted by the bound, a reset — ascending.
    std::vector<uint32_t> retiredCommandBlocks = {};
```

and `hasChanges()`'s return gains the two vectors:

```cpp
        return snapshot != 0 || !lines.empty() || titleChanged != 0 || cursorShapeChanged != 0
               || cwdChanged != 0 || colorsChanged != 0 || statusChanged != 0 || statusLinesChanged != 0
               || kittyKeyboardChanged != 0 || modifyOtherKeysChanged != 0 || mouseChanged != 0
               || progressChanged != 0 || contextChanged != 0 || !contexts.empty() || !commandBlocks.empty()
               || !retiredCommandBlocks.empty();
```

After the `estimatedEncodedSize(WireLine const&)` declaration (line 778):

```cpp
/// @return An upper bound on how many bytes @p block occupies once encoded.
///
/// Unlike the row estimate this one is a BOUND, never below the real size, because what it sizes is
/// not split further: it decides how many records one delta carries (NativeSession::
/// CommandBlockDeltaBytes) and how many a snapshot states, and an under-count there is a frame larger
/// than the budget it was sized against. `Pdu_test` pins it against a real `Writer`.
[[nodiscard]] std::size_t estimatedEncodedSize(WireCommandBlock const& block) noexcept;
```

- [ ] **Step 4: Encode, decode and estimate them**

In `src/vthost/proto/Pdu.cpp`, after `encodeContext` (line 349):

```cpp
    /// The optional fields of a command-block record, in WIRE ORDER, with the presence bit each is
    /// gated on — ONE table for both directions, for the reason ContextStringFields gives.
    constexpr auto CommandBlockOptionalFields = std::array {
        std::pair { CommandBlockHasExitCode, &WireCommandBlock::exitCode },
        std::pair { CommandBlockHasCommandStart, &WireCommandBlock::commandStartedAt },
        std::pair { CommandBlockHasDuration, &WireCommandBlock::duration },
    };

    void encodeCommandBlock(Writer& out, WireCommandBlock const& block)
    {
        out.varint(block.id);
        out.u8(block.state);
        out.u8(block.end);
        out.u8(block.commandLineSource);
        out.u8(block.locality);
        out.u8(block.present);
        out.string(block.commandLine);
        out.string(block.workingDirectory);
        out.u8(block.outcomeExit);
        out.u8(block.outcomeSignal);
        out.varint(block.outcomeStatus);
        out.svarint(block.promptStartedAt);
        for (auto const& [bit, member]: CommandBlockOptionalFields)
            if ((block.present & bit) != 0)
                out.svarint(block.*member);
    }
```

In `encodeBody(Writer&, SessionState const&)`, after `out.svarint(pdu.sessionEpoch);`:

```cpp
        out.varint(pdu.commandBlocks.size());
        for (auto const& block: pdu.commandBlocks)
            encodeCommandBlock(out, block);
```

In `encodeBody(Writer&, Delta const&)`, after the `contexts` loop (line 489):

```cpp
        out.varint(pdu.commandBlocks.size());
        for (auto const& block: pdu.commandBlocks)
            encodeCommandBlock(out, block);
        out.varint(pdu.retiredCommandBlocks.size());
        for (auto const id: pdu.retiredCommandBlocks)
            out.varint(id);
```

After `decodeContext` (line 726):

```cpp
    /// Decodes one command-block record, reading only the optionals its `present` mask names (after
    /// clearing the bits this build does not assign, so the stream stays in step). Enum-ish bytes are
    /// carried through raw; @see vthost::fromWireCommandBlock for where they are judged.
    [[nodiscard]] std::expected<WireCommandBlock, DecodeError> decodeCommandBlock(Reader& in)
    {
        auto block = WireCommandBlock {};
        auto error = DecodeError {};
        if (!assign(in.varint(), block.id, error) || !assign(in.u8(), block.state, error)
            || !assign(in.u8(), block.end, error) || !assign(in.u8(), block.commandLineSource, error)
            || !assign(in.u8(), block.locality, error) || !assign(in.u8(), block.present, error)
            || !assign(in.string(), block.commandLine, error)
            || !assign(in.string(), block.workingDirectory, error)
            || !assign(in.u8(), block.outcomeExit, error) || !assign(in.u8(), block.outcomeSignal, error)
            || !assign(in.varint(), block.outcomeStatus, error)
            || !assign(in.svarint(), block.promptStartedAt, error))
            return std::unexpected(error);
        block.present = static_cast<uint8_t>(block.present & CommandBlockPresentMask);
        for (auto const& [bit, member]: CommandBlockOptionalFields)
            if ((block.present & bit) != 0 && !assign(in.svarint(), block.*member, error))
                return std::unexpected(error);
        return block;
    }

    /// One host command-block id, range-checked into its 32 bits.
    [[nodiscard]] std::expected<uint32_t, DecodeError> decodeBlockId(Reader& in)
    {
        auto id = uint32_t {};
        auto error = DecodeError {};
        if (!assign(in.varint(), id, error))
            return std::unexpected(error);
        return id;
    }
```

In `decodeSessionState`, after the `sessionEpoch` read:

```cpp
        if (auto const decoded = decodeVector(in, pdu.commandBlocks, decodeCommandBlock); !decoded)
            return std::unexpected(decoded.error());
```

In `decodeDelta`, between the `contexts` decode (line 879-880) and `return pdu;`:

```cpp
        if (auto const decoded = decodeVector(in, pdu.commandBlocks, decodeCommandBlock); !decoded)
            return std::unexpected(decoded.error());
        if (auto const decoded = decodeVector(in, pdu.retiredCommandBlocks, decodeBlockId); !decoded)
            return std::unexpected(decoded.error());
```

After `estimatedEncodedSize(WireLine const&)` (line 1074):

```cpp
std::size_t estimatedEncodedSize(WireCommandBlock const& block) noexcept
{
    // encodeCommandBlock writes at most: a 5-byte varint id, five u8, two length varints (3 bytes each
    // up to 2 MiB), two u8, a 10-byte varint status, a 10-byte svarint start and three 10-byte
    // svarint optionals = 63, plus the two strings. Rounded up: this one must never under-count.
    constexpr auto FixedBytes = std::size_t { 72 };
    return FixedBytes + block.commandLine.size() + block.workingDirectory.size();
}
```

- [ ] **Step 5: Count them in the trace — never their contents**

In `src/vthost/proto/PduTrace.cpp`, the `SessionState` row's format becomes

```cpp
                       return std::format(
                           "session={} {}x{} screen={} title={}ch cwd={}ch progress={}/{} contexts={} "
                           "chain={} blocks={}",
```

with `value.commandBlocks.size()` appended to its arguments, and its comment gains: "Command-block records carry command lines and directories: counted, never shown." The `Delta` row's format string ends `"activecontext={} blocks={} retired={}"` with `value.commandBlocks.size(), value.retiredCommandBlocks.size()` appended to its arguments.

- [ ] **Step 6: Run the tests**

Run: `cmake --build --preset clangcl-debug --target vthost_test` then `out/build/clangcl-debug/bin/vthost_test.exe "[proto]"`
Expected: PASS (new cases, the catalog round trip, the trace cases, "an announced grid beyond MaxGridExtent is malformed").

- [ ] **Step 7: Commit**

```bash
clang-format -i src/vthost/proto/Pdu.hpp src/vthost/proto/Pdu.cpp src/vthost/proto/PduTrace.cpp src/vthost/proto/Pdu_test.cpp src/vthost/proto/PduTrace_test.cpp
git add src/vthost/proto/Pdu.hpp src/vthost/proto/Pdu.cpp src/vthost/proto/PduTrace.cpp src/vthost/proto/Pdu_test.cpp src/vthost/proto/PduTrace_test.cpp
git commit -F - <<'EOF'
vthost: add the command-block record to the native protocol

WireCommandBlock carries a block's record (minus its host-local head
position) in wire types. SessionState states them outright; Delta
carries the ones that changed and the ids the host retired. The trace
counts them and never prints a command line.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 2.6: Record ⇄ wire conversion — `vthost/CommandBlockWire.hpp`

Single-sourced like `vthost/ContextWire.hpp`, so the host's capture and the mirror's adoption cannot disagree. Implements D3's "head fields stay local".

**Files:**
- Create: `src/vthost/CommandBlockWire.hpp`, `src/vthost/CommandBlockWire_test.cpp`
- Modify: `src/vthost/CMakeLists.txt` (library list: after `ClientSizePolicy.hpp`; test list: after `ConnectionAcceptor_test.cpp`)

**Interfaces:**
- Consumes: C1 `CommandBlockRecord`, `CommandBlockState`, `CommandBlockEnd`, `CommandLineSource`, `WorkingDirectorySnapshot`, `ContextOutcome`, `ContextLocality`; `vthost::contextExitOf`, `vthost::contextSignalOf` (`src/vthost/ContextWire.hpp:37-53`); Task 2.4's `TimeWire.hpp`; Task 2.5's `proto::WireCommandBlock`.
- Produces (namespace `vthost`):
  - `constexpr inline uint64_t UnknownHeadGeneration = std::numeric_limits<uint64_t>::max();`
  - `[[nodiscard]] constexpr std::optional<vtbackend::CommandBlockState> commandBlockStateOf(uint8_t) noexcept;` and likewise `commandBlockEndOf`, `commandLineSourceOf`, `contextLocalityOf`
  - `[[nodiscard]] constexpr uint8_t presenceOf(vtbackend::CommandBlockRecord const&) noexcept;`
  - `[[nodiscard]] proto::WireCommandBlock toWireCommandBlock(vtbackend::CommandBlockRecord const& record);`
  - `[[nodiscard]] vtbackend::CommandBlockRecord fromWireCommandBlock(proto::WireCommandBlock const& wire, vtbackend::CommandBlockRecord const* held);`
  - `[[nodiscard]] bool matchesWireCommandBlock(vtbackend::CommandBlockRecord const& record, proto::WireCommandBlock const& wire) noexcept;`

- [ ] **Step 1: Write the failing test**

Create `src/vthost/CommandBlockWire_test.cpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/shell/CommandBlock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <vector>

#include <vthost/CommandBlockWire.hpp>

using namespace std::chrono_literals;
namespace proto = vthost::proto;

namespace
{

/// A finished record with every optional field present and every enum off its zero value.
[[nodiscard]] vtbackend::CommandBlockRecord fullRecord()
{
    auto record = vtbackend::CommandBlockRecord {};
    record.id = vtbackend::CommandBlockId { 42 };
    record.state = vtbackend::CommandBlockState::Finished;
    record.end = vtbackend::CommandBlockEnd::Implicit;
    record.commandLine = "make test";
    record.commandLineSource = vtbackend::CommandLineSource::Recovered;
    record.exitCode = 2;
    record.outcome.exit = vtbackend::ContextExit::Crash;
    record.outcome.signal = vtbackend::ContextSignal::Segv;
    record.outcome.status = 139;
    record.promptStartedAt = std::chrono::system_clock::time_point { 1'700'000'000s };
    record.commandStartedAt = record.promptStartedAt + 2s;
    record.duration = std::chrono::duration_cast<std::chrono::steady_clock::duration>(3s);
    record.workingDirectory.path = "/home/user/src";
    record.workingDirectory.locality = vtbackend::ContextLocality::Local;
    record.headStableId = 1234; // the HOST's row id: must never reach the wire
    record.headIdGeneration = 5;
    return record;
}

} // namespace

TEST_CASE("a record survives its wire form, minus the host's head position", "[vthost][wire][blocks]")
{
    auto const record = fullRecord();
    auto const back = vthost::fromWireCommandBlock(vthost::toWireCommandBlock(record), nullptr);

    auto expected = record;
    expected.headStableId = 0;
    expected.headIdGeneration = vthost::UnknownHeadGeneration; // no grid reaches it: "not yet known"
    CHECK(back == expected);
}

TEST_CASE("a record the mirror already holds keeps the mirror's own head position", "[vthost][wire][blocks]")
{
    auto held = fullRecord();
    held.headStableId = 77;
    held.headIdGeneration = 3;
    auto changed = vthost::toWireCommandBlock(fullRecord());
    changed.commandLine = "make install";

    auto const back = vthost::fromWireCommandBlock(changed, &held);
    CHECK(back.commandLine == "make install");
    CHECK(back.headStableId == 77);
    CHECK(back.headIdGeneration == 3);
}

TEST_CASE("bytes no enumerator names fall to the zero value", "[vthost][wire][blocks]")
{
    // A peer's byte becomes an enumerator here or not at all: every switch downstream assumes it
    // holds one the enum has.
    auto wire = proto::WireCommandBlock {};
    wire.state = 9;
    wire.end = 7;
    wire.commandLineSource = 5;
    wire.locality = 9;
    wire.outcomeExit = 99;
    wire.outcomeSignal = 200;

    auto const record = vthost::fromWireCommandBlock(wire, nullptr);
    CHECK(record.state == vtbackend::CommandBlockState::Prompting);
    CHECK(record.end == vtbackend::CommandBlockEnd::Reported);
    CHECK(record.commandLineSource == vtbackend::CommandLineSource::None);
    CHECK(record.workingDirectory.locality == vtbackend::ContextLocality::Unknown);
    CHECK(record.outcome.exit == vtbackend::ContextExit::Unknown);
    CHECK(record.outcome.signal == vtbackend::ContextSignal::None);
}

TEST_CASE("an optional the wire marks absent, or cannot represent, is absent", "[vthost][wire][blocks]")
{
    auto wire = vthost::toWireCommandBlock(fullRecord());
    wire.exitCode = std::numeric_limits<int64_t>::max(); // present, but no `int` holds it
    wire.present = static_cast<uint8_t>(wire.present & ~proto::CommandBlockHasDuration);

    auto const record = vthost::fromWireCommandBlock(wire, nullptr);
    CHECK_FALSE(record.exitCode.has_value());
    CHECK_FALSE(record.duration.has_value());
    CHECK(record.commandStartedAt.has_value());
}

TEST_CASE("matchesWireCommandBlock notices every field the wire carries, and only those", "[vthost][wire][blocks]")
{
    auto const record = fullRecord();
    auto const wire = vthost::toWireCommandBlock(record);
    REQUIRE(vthost::matchesWireCommandBlock(record, wire));

    // One mutation per wire field: the comparison stands in for "send it again?", so a field it
    // forgot would be a change the client never hears about.
    auto const mutations = std::vector<std::function<void(proto::WireCommandBlock&)>> {
        [](auto& w) { w.id += 1; },
        [](auto& w) { w.state = 1; },
        [](auto& w) { w.end = 0; },
        [](auto& w) { w.commandLineSource = 1; },
        [](auto& w) { w.locality = 2; },
        [](auto& w) { w.present = static_cast<uint8_t>(w.present & ~proto::CommandBlockHasExitCode); },
        [](auto& w) { w.commandLine += "x"; },
        [](auto& w) { w.workingDirectory += "x"; },
        [](auto& w) { w.outcomeExit = 1; },
        [](auto& w) { w.outcomeSignal = 9; },
        [](auto& w) { w.outcomeStatus += 1; },
        [](auto& w) { w.promptStartedAt += 1000; },
        [](auto& w) { w.exitCode += 1; },
        [](auto& w) { w.commandStartedAt += 1000; },
        [](auto& w) { w.duration += 1000; },
    };
    for (auto const& mutate: mutations)
    {
        auto changed = wire;
        mutate(changed);
        CHECK_FALSE(vthost::matchesWireCommandBlock(record, changed));
    }

    // The head position is not the wire's business: moving it is no reason to re-send.
    auto moved = record;
    moved.headStableId = 9999;
    moved.headIdGeneration = 9;
    CHECK(vthost::matchesWireCommandBlock(moved, wire));
}
```

Register it in `src/vthost/CMakeLists.txt`'s `vthost_test` list after `ConnectionAcceptor_test.cpp`:

```cmake
        CommandBlockWire_test.cpp
```

- [ ] **Step 2: Run it to see it fail**

Run: `cmake --build --preset clangcl-debug --target vthost_test`
Expected: FAIL — does not compile (`vthost/CommandBlockWire.hpp` not found).

- [ ] **Step 3: Write the conversion header**

Create `src/vthost/CommandBlockWire.hpp`:

```cpp
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A command block's record as it travels on the native protocol — single-sourced so the host's
/// capture (NativeSession) and the mirror's adoption (ScreenMirror) cannot disagree, exactly as
/// @ref vthost/ContextWire.hpp does for OSC 3008 contexts.
///
/// Decoding is TOTAL: four enums and a presence mask arrive as bytes a peer chose, and none of them is
/// cast into its enum unchecked. The record's cached head position never crosses the wire (@see
/// proto::WireCommandBlock): a mirror keeps its own.

#include <vtbackend/core/TerminalContext.hpp>
#include <vtbackend/shell/CommandBlock.hpp>

#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

#include <vthost/ContextWire.hpp>
#include <vthost/TimeWire.hpp>
#include <vthost/proto/Pdu.hpp>

namespace vthost
{

/// The head generation a mirrored record carries until the mirror has written its head row: one no
/// grid ever reaches, so a consumer validating the cached head against `Grid::stableIdGeneration()` treats it
/// as unknown — and rescans — instead of trusting a row id from the HOST's id space.
constexpr inline uint64_t UnknownHeadGeneration = std::numeric_limits<uint64_t>::max();

// The record's enums travel as raw bytes, so their numbering is frozen here. Renumbering one in
// CommandBlock.hpp then fails to compile at the boundary, where the question "does the wire change?"
// has to be answered, instead of a peer silently reading Running as Finished.
static_assert(std::to_underlying(vtbackend::CommandBlockState::Prompting) == 0);
static_assert(std::to_underlying(vtbackend::CommandBlockState::Running) == 1);
static_assert(std::to_underlying(vtbackend::CommandBlockState::Finished) == 2);
static_assert(std::to_underlying(vtbackend::CommandBlockEnd::Reported) == 0);
static_assert(std::to_underlying(vtbackend::CommandBlockEnd::Implicit) == 1);
static_assert(std::to_underlying(vtbackend::CommandLineSource::None) == 0);
static_assert(std::to_underlying(vtbackend::CommandLineSource::Reported) == 1);
static_assert(std::to_underlying(vtbackend::CommandLineSource::Recovered) == 2);
static_assert(std::to_underlying(vtbackend::ContextLocality::Unknown) == 0);
static_assert(std::to_underlying(vtbackend::ContextLocality::Foreign) == 2);

namespace detail
{
    /// @return @p value as an enumerator of a contiguous enum ending at @p last, or nullopt. An
    ///         enumerator added past @p last fails CLOSED (reads as unknown) until @p last moves.
    template <typename E>
    [[nodiscard]] constexpr std::optional<E> enumUpTo(uint8_t value, E last) noexcept
    {
        return value <= std::to_underlying(last) ? std::optional { static_cast<E>(value) } : std::nullopt;
    }
} // namespace detail

/// @return The state @p value names, or nullopt.
[[nodiscard]] constexpr std::optional<vtbackend::CommandBlockState> commandBlockStateOf(uint8_t value) noexcept
{
    return detail::enumUpTo(value, vtbackend::CommandBlockState::Finished);
}

/// @return The end @p value names, or nullopt.
[[nodiscard]] constexpr std::optional<vtbackend::CommandBlockEnd> commandBlockEndOf(uint8_t value) noexcept
{
    return detail::enumUpTo(value, vtbackend::CommandBlockEnd::Implicit);
}

/// @return The command-line source @p value names, or nullopt.
[[nodiscard]] constexpr std::optional<vtbackend::CommandLineSource> commandLineSourceOf(uint8_t value) noexcept
{
    return detail::enumUpTo(value, vtbackend::CommandLineSource::Recovered);
}

/// @return The locality @p value names, or nullopt.
[[nodiscard]] constexpr std::optional<vtbackend::ContextLocality> contextLocalityOf(uint8_t value) noexcept
{
    return detail::enumUpTo(value, vtbackend::ContextLocality::Foreign);
}

/// @return The CommandBlockHas* bits naming which of @p record's optionals are engaged.
[[nodiscard]] constexpr uint8_t presenceOf(vtbackend::CommandBlockRecord const& record) noexcept
{
    auto present = uint8_t { 0 };
    if (record.exitCode.has_value())
        present = static_cast<uint8_t>(present | proto::CommandBlockHasExitCode);
    if (record.commandStartedAt.has_value())
        present = static_cast<uint8_t>(present | proto::CommandBlockHasCommandStart);
    if (record.duration.has_value())
        present = static_cast<uint8_t>(present | proto::CommandBlockHasDuration);
    return present;
}

/// The wire form of @p record, for a host replicating it. The head position stays behind.
[[nodiscard]] inline proto::WireCommandBlock toWireCommandBlock(vtbackend::CommandBlockRecord const& record)
{
    auto wire = proto::WireCommandBlock {};
    wire.id = record.id.value;
    wire.state = std::to_underlying(record.state);
    wire.end = std::to_underlying(record.end);
    wire.commandLineSource = std::to_underlying(record.commandLineSource);
    wire.locality = std::to_underlying(record.workingDirectory.locality);
    wire.present = presenceOf(record);
    wire.commandLine = record.commandLine;
    wire.workingDirectory = record.workingDirectory.path;
    wire.outcomeExit = std::to_underlying(record.outcome.exit);
    wire.outcomeSignal = std::to_underlying(record.outcome.signal);
    wire.outcomeStatus = record.outcome.status;
    wire.promptStartedAt = toWireTime(record.promptStartedAt);
    wire.exitCode = record.exitCode.value_or(0);
    wire.commandStartedAt = record.commandStartedAt ? toWireTime(*record.commandStartedAt) : 0;
    wire.duration = record.duration ? toWireDuration(*record.duration) : 0;
    return wire;
}

/// Builds the record a mirror adopts from @p wire.
///
/// Total: every enum-ish byte goes through a validator and falls to its zero value when it names
/// nothing; an exit code no `int` holds, or a negative duration, is treated as absent.
/// @param wire The record off the wire.
/// @param held The mirror's current copy of this id, or nullptr. Its head position is kept — it names
///        a row in the MIRROR's grid; without one the head is @ref UnknownHeadGeneration.
[[nodiscard]] inline vtbackend::CommandBlockRecord fromWireCommandBlock(proto::WireCommandBlock const& wire,
                                                                       vtbackend::CommandBlockRecord const* held)
{
    auto record = vtbackend::CommandBlockRecord {};
    record.id = vtbackend::CommandBlockId { wire.id };
    record.state = commandBlockStateOf(wire.state).value_or(vtbackend::CommandBlockState::Prompting);
    record.end = commandBlockEndOf(wire.end).value_or(vtbackend::CommandBlockEnd::Reported);
    record.commandLine = wire.commandLine;
    record.commandLineSource = commandLineSourceOf(wire.commandLineSource).value_or(vtbackend::CommandLineSource::None);
    if ((wire.present & proto::CommandBlockHasExitCode) != 0 && std::in_range<int>(wire.exitCode))
        record.exitCode = static_cast<int>(wire.exitCode);
    record.outcome.exit = contextExitOf(wire.outcomeExit).value_or(vtbackend::ContextExit::Unknown);
    record.outcome.signal = contextSignalOf(wire.outcomeSignal).value_or(vtbackend::ContextSignal::None);
    record.outcome.status = wire.outcomeStatus;
    record.promptStartedAt = fromWireTime(wire.promptStartedAt);
    if ((wire.present & proto::CommandBlockHasCommandStart) != 0)
        record.commandStartedAt = fromWireTime(wire.commandStartedAt);
    if ((wire.present & proto::CommandBlockHasDuration) != 0 && wire.duration >= 0)
        record.duration = fromWireDuration(wire.duration);
    record.workingDirectory.path = wire.workingDirectory;
    record.workingDirectory.locality = contextLocalityOf(wire.locality).value_or(vtbackend::ContextLocality::Unknown);
    if (held != nullptr)
    {
        record.headStableId = held->headStableId;
        record.headIdGeneration = held->headIdGeneration;
    }
    else
        record.headIdGeneration = UnknownHeadGeneration;
    return record;
}

/// Whether @p record already holds exactly what @p wire carries — every wire field, and nothing else
/// (the head position is not compared).
///
/// Field by field rather than `toWireCommandBlock(record) == wire`: the host asks this of every record
/// on every walk, and building the wire form to ask would copy two strings per record to usually learn
/// nothing moved. Scalars first, strings last. Mirrors @ref toWireCommandBlock line for line,
/// deliberately; a field in one and not the other is the bug this exists to prevent.
[[nodiscard]] inline bool matchesWireCommandBlock(vtbackend::CommandBlockRecord const& record,
                                                  proto::WireCommandBlock const& wire) noexcept
{
    return record.id.value == wire.id && std::to_underlying(record.state) == wire.state
           && std::to_underlying(record.end) == wire.end
           && std::to_underlying(record.commandLineSource) == wire.commandLineSource
           && std::to_underlying(record.workingDirectory.locality) == wire.locality
           && presenceOf(record) == wire.present && std::to_underlying(record.outcome.exit) == wire.outcomeExit
           && std::to_underlying(record.outcome.signal) == wire.outcomeSignal
           && record.outcome.status == wire.outcomeStatus
           && toWireTime(record.promptStartedAt) == wire.promptStartedAt
           && record.exitCode.value_or(0) == wire.exitCode
           && (record.commandStartedAt ? toWireTime(*record.commandStartedAt) : 0) == wire.commandStartedAt
           && (record.duration ? toWireDuration(*record.duration) : 0) == wire.duration
           && record.commandLine == wire.commandLine && record.workingDirectory.path == wire.workingDirectory;
}

} // namespace vthost
```

Add `CommandBlockWire.hpp` to the `vthost` library list in `src/vthost/CMakeLists.txt`, after `ClientSizePolicy.hpp`.

- [ ] **Step 4: Run the tests**

Run: `cmake --build --preset clangcl-debug --target vthost_test` then `out/build/clangcl-debug/bin/vthost_test.exe "[wire]"`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
clang-format -i src/vthost/CommandBlockWire.hpp src/vthost/CommandBlockWire_test.cpp
git add src/vthost/CommandBlockWire.hpp src/vthost/CommandBlockWire_test.cpp src/vthost/CMakeLists.txt
git commit -F - <<'EOF'
vthost: convert command-block records to and from the wire

One header both ends use: the record's wire form without its host-local
head position, a total decode that turns unknown bytes into zero values,
and an allocation-free "has anything the wire carries moved?" check.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 2.7: Host side — per-connection, revision-gated record replication in `NativeSession`

Implements D1, D2 and D5's host half.

**Files:**
- Modify: `src/vthost/NativeSession.hpp:66` (constant), `:202` (`FollowState`), `:283-287` (`BuiltPush` + new enum), `:369-372` (declaration after `collectContextState`)
- Modify: `src/vthost/NativeSession.cpp:25-33` (include), `:355` (definition after `collectContextState`), `:567-572`, `:716`, `:745` (`buildPush`), `:748-762` (`pushDelta`), `:815-822` (`streamSnapshots`)
- Test: `src/vthost/NativeSession_test.cpp` (helpers appended to the anonymous namespace ending :385; cases appended at end of file)

**Interfaces:**
- Consumes: C1 `Terminal::commandBlocks()`, `CommandBlockStore::revision()`, `forEachRecord()`, `find()`, `size()`; Task 2.5 wire fields; Task 2.6 `toWireCommandBlock`, `matchesWireCommandBlock`.
- Produces:
  - `static constexpr std::size_t NativeSession::CommandBlockDeltaBytes = 64 * 1024;` (public)
  - `enum class NativeSession::CommandBlockBacklog : uint8_t { Drained = 0, Pending };` (private)
  - `[[nodiscard]] static CommandBlockBacklog NativeSession::collectCommandBlockState(vtbackend::Terminal&, FollowState&, proto::Delta&, std::optional<proto::SessionState>&, SnapshotMode);` (private)
  - Wire behaviour: a snapshot's `SessionState::commandBlocks` holds the newest records within `SnapshotChunkBytes`; each later `Delta` holds the records that differ from what this connection was sent (oldest first, at most `CommandBlockDeltaBytes` unless one record alone is larger) and the ids the host no longer holds.

- [ ] **Step 1: Write the failing tests**

Append to the anonymous namespace of `src/vthost/NativeSession_test.cpp` (before its closing `} // namespace` at line 385):

```cpp
/// One whole command cycle on the CURRENT line — A, C with @p commandLine as kitty's cmdline_url, D
/// with @p exitCode — writing no newline, so a burst of them scrolls nothing and stays an increment
/// even with the harness's zero scrollback.
std::string commandCycle(std::string_view commandLine, int exitCode)
{
    return std::format(
        "\033]133;A\033\\\033]133;C;cmdline_url={}\033\\\033]133;D;{}\033\\", commandLine, exitCode);
}

/// Writes @p bytes after @p delay and kicks the debounced flush. By value: a coroutine frame outlives
/// the full-expression that created it.
Task<void> writeAfter(NativeHarness* h,
                      vtworkspace::SessionId id,
                      std::string bytes,
                      std::chrono::milliseconds delay)
{
    co_await h->loop.delay(delay);
    h->host.terminal(id)->writeToScreen(bytes);
    h->session->sessionScreenUpdated(id);
}

/// @return The host's command-block ids, oldest first.
std::vector<uint32_t> hostBlockIds(NativeHarness& h, vtworkspace::SessionId id)
{
    auto ids = std::vector<uint32_t> {};
    h.host.terminal(id)->commandBlocks().forEachRecord(
        [&](vtbackend::CommandBlockRecord const& record) { ids.push_back(record.id.value); });
    return ids;
}

/// @return The incremental (non-snapshot) deltas in @p frames, in arrival order.
std::vector<proto::Delta const*> increments(std::vector<proto::DecodedFrame> const& frames)
{
    auto deltas = std::vector<proto::Delta const*> {};
    for (auto const& frame: frames)
        if (auto const* delta = std::get_if<proto::Delta>(&frame.pdu); delta != nullptr && delta->snapshot == 0)
            deltas.push_back(delta);
    return deltas;
}

/// @return Every command-block id @p frames delivered, by SessionState or by Delta.
std::set<uint32_t> deliveredBlockIds(std::vector<proto::DecodedFrame> const& frames)
{
    auto ids = std::set<uint32_t> {};
    for (auto const& frame: frames)
    {
        if (auto const* state = std::get_if<proto::SessionState>(&frame.pdu))
            for (auto const& block: state->commandBlocks)
                ids.insert(block.id);
        if (auto const* delta = std::get_if<proto::Delta>(&frame.pdu))
            for (auto const& block: delta->commandBlocks)
                ids.insert(block.id);
    }
    return ids;
}

/// Collects until @p count distinct command-block ids have arrived.
Task<void> collectBlocks(core::net::ISocket* client, std::size_t count, std::vector<proto::DecodedFrame>* out)
{
    co_await collectUntil(
        client, [count](auto const& got) { return deliveredBlockIds(got).size() >= count; }, out);
}

/// Closes the client end if @p count ids have not arrived within ~3 s, so a regression fails the
/// count instead of hanging the suite.
Task<void> closeUnlessDelivered(NativeHarness* h, std::vector<proto::DecodedFrame> const* frames, std::size_t count)
{
    if (!co_await core::net::testing::waitUntil(
            &h->loop, [frames, count] { return deliveredBlockIds(*frames).size() >= count; }, 3000))
        h->pair.second->close();
}
```

Add `#include <vtbackend/shell/CommandBlock.hpp>` after `#include <vtbackend/core/Primitives.hpp>`, then append at the end of the file:

```cpp
TEST_CASE("the attach snapshot states every command block outright", "[vthost][native][blocks]")
{
    auto h = NativeHarness {};
    h.host.createTab();
    auto const sessionId = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    h.host.terminal(sessionId)->writeToScreen(commandCycle("make%20test", 2));

    // Expect: ServerHello, LayoutState, SessionState, Delta (snapshot).
    auto const received = h.exchange({ proto::ClientHello {} }, 4);
    REQUIRE(received.size() == 4);
    auto const* state = std::get_if<proto::SessionState>(&received[2].pdu);
    REQUIRE(state != nullptr);
    REQUIRE(state->commandBlocks.size() == 1);
    auto const& block = state->commandBlocks.front();
    CHECK(block.id == hostBlockIds(h, sessionId).front());
    CHECK(block.state == std::to_underlying(vtbackend::CommandBlockState::Finished));
    CHECK(block.commandLine == "make test");
    CHECK((block.present & proto::CommandBlockHasExitCode) != 0);
    CHECK(block.exitCode == 2);

    // Stated once, in SessionState — the snapshot's rows do not repeat it.
    auto const* snapshot = std::get_if<proto::Delta>(&received[3].pdu);
    REQUIRE(snapshot != nullptr);
    CHECK(snapshot->commandBlocks.empty());
}

TEST_CASE("an increment carries only the command blocks that changed", "[vthost][native][blocks]")
{
    auto h = NativeHarness {};
    h.host.createTab();
    auto const sessionId = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    h.host.terminal(sessionId)->writeToScreen(commandCycle("ls", 0));

    auto const bytes = encodeRequest({ proto::DecodedPdu { proto::ClientHello {} } });
    auto received = std::vector<proto::DecodedFrame> {};
    h.loop.blockOn(core::net::testing::allOf(h.session->run(),
                                             feedBytes(h.pair.second.get(), &bytes),
                                             collectPdus(h.pair.second.get(), 6, &received),
                                             writeAfter(&h, sessionId, "\r\n\033]133;A\033\\$ ", 5ms),
                                             writeAfter(&h, sessionId, "typed", 60ms)));
    REQUIRE(received.size() == 6);

    // [4] the new prompt: exactly one record, and it is not the finished one again.
    auto const ids = hostBlockIds(h, sessionId);
    REQUIRE(ids.size() == 2);
    auto const* prompt = std::get_if<proto::Delta>(&received[4].pdu);
    REQUIRE(prompt != nullptr);
    REQUIRE(prompt->snapshot == 0);
    REQUIRE(prompt->commandBlocks.size() == 1);
    CHECK(prompt->commandBlocks.front().id == ids.back());
    CHECK(prompt->commandBlocks.front().state == std::to_underlying(vtbackend::CommandBlockState::Prompting));

    // [5] typing at the prompt moved a row and no record: the revision gate kept it record-free.
    auto const* typing = std::get_if<proto::Delta>(&received[5].pdu);
    REQUIRE(typing != nullptr);
    CHECK_FALSE(typing->lines.empty());
    CHECK(typing->commandBlocks.empty());
    CHECK(typing->retiredCommandBlocks.empty());
}

TEST_CASE("a prompt the host discarded is retired on the client", "[vthost][native][blocks]")
{
    // An empty Enter: the next OSC 133;A discards the Prompting record that never reached C. The
    // client was told about it, so it must be told it is gone — or an attached pane keeps a phantom
    // block for every empty prompt.
    auto h = NativeHarness {};
    h.host.createTab();
    auto const sessionId = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    h.host.terminal(sessionId)->writeToScreen("\033]133;A\033\\$ ");
    auto const discarded = hostBlockIds(h, sessionId);
    REQUIRE(discarded.size() == 1);

    auto const bytes = encodeRequest({ proto::DecodedPdu { proto::ClientHello {} } });
    auto received = std::vector<proto::DecodedFrame> {};
    h.loop.blockOn(core::net::testing::allOf(h.session->run(),
                                             feedBytes(h.pair.second.get(), &bytes),
                                             collectPdus(h.pair.second.get(), 5, &received),
                                             writeAfter(&h, sessionId, "\r\n\033]133;A\033\\$ ", 5ms)));
    REQUIRE(received.size() == 5);

    auto const remaining = hostBlockIds(h, sessionId);
    REQUIRE(remaining.size() == 1);
    REQUIRE(remaining.front() != discarded.front());
    auto const* delta = std::get_if<proto::Delta>(&received[4].pdu);
    REQUIRE(delta != nullptr);
    CHECK(delta->retiredCommandBlocks == std::vector<uint32_t> { discarded.front() });
    REQUIRE(delta->commandBlocks.size() == 1);
    CHECK(delta->commandBlocks.front().id == remaining.front());
}

TEST_CASE("a burst of records larger than one delta's budget drains oldest first", "[vthost][native][blocks]")
{
    // Forty 4 KB command lines are ~160 KB of records: built into one delta they would be a frame the
    // send queue may refuse on a non-empty backlog. Budgeted, they arrive over several deltas, each
    // within CommandBlockDeltaBytes, oldest first, each exactly once.
    auto h = NativeHarness {};
    h.host.createTab();
    auto const sessionId = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    auto const longCommand = std::string(4000, 'a');
    auto burst = std::string {};
    for (auto const exitCode: std::views::iota(0, 40))
        burst += commandCycle(longCommand, exitCode);

    auto const bytes = encodeRequest({ proto::DecodedPdu { proto::ClientHello {} } });
    auto received = std::vector<proto::DecodedFrame> {};
    h.loop.blockOn(core::net::testing::allOf(h.session->run(),
                                             feedBytes(h.pair.second.get(), &bytes),
                                             collectBlocks(h.pair.second.get(), 40, &received),
                                             closeUnlessDelivered(&h, &received, 40),
                                             writeAfter(&h, sessionId, burst, 5ms)));

    auto delivered = std::vector<uint32_t> {};
    auto carrying = 0;
    for (auto const* delta: increments(received))
    {
        auto recordBytes = std::size_t { 0 };
        for (auto const& block: delta->commandBlocks)
        {
            recordBytes += proto::estimatedEncodedSize(block);
            delivered.push_back(block.id);
        }
        CHECK((delta->commandBlocks.size() <= 1 || recordBytes <= NativeSession::CommandBlockDeltaBytes));
        carrying += delta->commandBlocks.empty() ? 0 : 1;
    }
    CHECK(delivered == hostBlockIds(h, sessionId)); // every record, once, oldest first
    CHECK(carrying > 1);                             // and it really took several deltas
}

TEST_CASE("a snapshot states the newest records within its cap; the rest follow", "[vthost][native][blocks]")
{
    // A hundred 4 KB records (~410 KB) exceed SnapshotChunkBytes. SessionState is one unchunked frame,
    // so it states the newest that fit, and the older remainder arrives as increments right after.
    auto h = NativeHarness {};
    h.host.createTab();
    auto const sessionId = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    auto const longCommand = std::string(4000, 'b');
    auto burst = std::string {};
    for (auto const exitCode: std::views::iota(0, 100))
        burst += commandCycle(longCommand, exitCode % 256);
    h.host.terminal(sessionId)->writeToScreen(burst);
    auto const ids = hostBlockIds(h, sessionId);
    REQUIRE(ids.size() == 100);

    auto const bytes = encodeRequest({ proto::DecodedPdu { proto::ClientHello {} } });
    auto received = std::vector<proto::DecodedFrame> {};
    h.loop.blockOn(core::net::testing::allOf(h.session->run(),
                                             feedBytes(h.pair.second.get(), &bytes),
                                             collectBlocks(h.pair.second.get(), 100, &received),
                                             closeUnlessDelivered(&h, &received, 100)));

    auto const* state = lastSessionState(received);
    REQUIRE(state != nullptr);
    REQUIRE_FALSE(state->commandBlocks.empty());
    auto stated = std::size_t { 0 };
    for (auto const& block: state->commandBlocks)
        stated += proto::estimatedEncodedSize(block);
    CHECK(stated <= NativeSession::SnapshotChunkBytes);
    CHECK(state->commandBlocks.back().id == ids.back());   // the newest made it ...
    CHECK(state->commandBlocks.front().id != ids.front()); // ... the oldest did not ...
    CHECK(deliveredBlockIds(received) == std::set<uint32_t>(ids.begin(), ids.end())); // ... but followed
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset clangcl-debug --target vthost_test` then `out/build/clangcl-debug/bin/vthost_test.exe "[native][blocks]"`
Expected: FAIL — does not compile (`'CommandBlockDeltaBytes': is not a member of 'vthost::NativeSession'`). With that constant stubbed in, every case fails on empty `commandBlocks`.

- [ ] **Step 3: Declare the state**

In `src/vthost/NativeSession.hpp`, after `SnapshotChunkBytes` (line 66):

```cpp
    /// The most command-block record bytes one INCREMENTAL delta carries (by
    /// proto::estimatedEncodedSize, which never under-counts).
    ///
    /// An increment is emitted inline, unpaced, so whatever it carries must fit beside a backlog. A
    /// flood of OSC 133 cycles — a `cat` of a recorded session — can change a whole store's worth of
    /// records within one debounce window, at up to MaxRecordedCommandLineBytes each, which unbounded
    /// is megabytes in one frame. Budgeted, the remainder waits for the next window: records arrive
    /// late, never lost, and never at the cost of the connection.
    static constexpr std::size_t CommandBlockDeltaBytes = std::size_t { 64 } * 1024;
```

In `FollowState`, after `int lastActiveContext = -1;` (line 202):

```cpp
        /// The command-block records this peer has been told about, keyed by host id, as the wire
        /// record last sent. Compared field by field against the live record (@see
        /// vthost::matchesWireCommandBlock), so a record is re-sent exactly when something the wire
        /// carries moved — a record changes in place under one id (C, then D, then an OSC 3008
        /// enrichment), which is why this keeps the whole record rather than the id alone.
        std::unordered_map<uint32_t, proto::WireCommandBlock> sentCommandBlocks;
        /// The CommandBlockStore revision `sentCommandBlocks` was last brought fully in line with;
        /// nullopt while the next walk must run regardless — never walked yet, or a budget or cap left
        /// records unsent.
        std::optional<uint64_t> lastCommandBlockRevision;
```

Replace `BuiltPush` (lines 283-287) with:

```cpp
    /// Whether a push said everything this peer is owed about command-block records.
    enum class CommandBlockBacklog : uint8_t
    {
        Drained = 0, ///< Every record this peer has not seen travelled.
        Pending = 1, ///< A budget or cap ran out; the rest go in the next delta.
    };

    struct BuiltPush
    {
        proto::Delta delta;                       ///< The rows and state to send.
        std::optional<proto::SessionState> state; ///< Precedes the delta, on a snapshot.
        /// Pending when records were left for the next delta; the caller re-queues the session.
        CommandBlockBacklog blockBacklog = CommandBlockBacklog::Drained;
    };
```

After the `collectContextState` declaration (line 372):

```cpp
    /// Captures the command-block records this peer has not seen in their current form.
    ///
    /// On a snapshot (@p mode Forced) the records go into @p state — the newest whose estimated size
    /// fits SnapshotChunkBytes, oldest first — and `sentCommandBlocks` becomes exactly that set. On an
    /// increment the walk runs only when the store's revision moved since the last complete one; it
    /// sends, oldest first, each record that differs from its last-sent copy until
    /// CommandBlockDeltaBytes is spent (always at least one), and lists as retired every sent id the
    /// store no longer holds.
    ///
    /// Cost: nothing while the revision is unchanged; otherwise one pass over at most `maxRecords`
    /// records, scalars compared before strings, plus one `find` per id this peer holds.
    /// Static: it reads only its arguments.
    /// @return Pending when records were left unsent (the caller re-queues the session).
    [[nodiscard]] static CommandBlockBacklog collectCommandBlockState(vtbackend::Terminal& terminal,
                                                                      FollowState& follow,
                                                                      proto::Delta& delta,
                                                                      std::optional<proto::SessionState>& state,
                                                                      SnapshotMode mode);
```

- [ ] **Step 4: Implement the walk**

In `src/vthost/NativeSession.cpp`, add `#include <vthost/CommandBlockWire.hpp>` before `#include <vthost/ContextWire.hpp>`, and after `collectContextState` (line 355):

```cpp
NativeSession::CommandBlockBacklog NativeSession::collectCommandBlockState(
    vtbackend::Terminal& terminal,
    FollowState& follow,
    proto::Delta& delta,
    std::optional<proto::SessionState>& state,
    SnapshotMode mode)
{
    auto const& store = terminal.commandBlocks();

    if (mode == SnapshotMode::Forced)
    {
        // Stated outright, like the context pool — but capped, because SessionState is one unchunked
        // frame and a store of hostile 4 KB command lines would otherwise make it megabytes. The
        // NEWEST records are the ones a client wants first (the current prompt, the last finish);
        // the older remainder is left unsent, so the next walk finds it and the increments carry it.
        auto all = std::vector<proto::WireCommandBlock> {};
        all.reserve(store.size());
        store.forEachRecord(
            [&](vtbackend::CommandBlockRecord const& record) { all.push_back(toWireCommandBlock(record)); });
        auto kept = std::size_t { 0 };
        auto spent = std::size_t { 0 };
        for (auto const& wire: all | std::views::reverse)
        {
            auto const cost = proto::estimatedEncodedSize(wire);
            if (kept != 0 && spent + cost > SnapshotChunkBytes)
                break;
            spent += cost;
            ++kept;
        }
        // Rebuilt to exactly what this snapshot states: the client REPLACES its table with it.
        follow.sentCommandBlocks.clear();
        for (auto& wire: all | std::views::drop(static_cast<std::ptrdiff_t>(all.size() - kept)))
        {
            follow.sentCommandBlocks.insert_or_assign(wire.id, wire);
            if (state)
                state->commandBlocks.push_back(std::move(wire));
        }
        auto const capped = kept < all.size();
        follow.lastCommandBlockRevision =
            capped ? std::nullopt : std::optional<uint64_t> { store.revision() };
        return capped ? CommandBlockBacklog::Pending : CommandBlockBacklog::Drained;
    }

    // The revision gate, as for contexts: a debounce flush runs every 20 ms per session, and the walk
    // below can have nothing to say while the store has not moved.
    if (follow.lastCommandBlockRevision == store.revision())
        return CommandBlockBacklog::Drained;

    auto spent = std::size_t { 0 };
    auto backlog = CommandBlockBacklog::Drained;
    store.forEachRecord([&](vtbackend::CommandBlockRecord const& record) {
        // Once over budget, everything newer waits too, so the peer still learns of records oldest
        // first — which is what lets its mirror tell a finish it witnessed from history it was never
        // shown (@see ScreenMirror::syncCommandBlocks).
        if (backlog == CommandBlockBacklog::Pending)
            return;
        auto const known = follow.sentCommandBlocks.find(record.id.value);
        if (known != follow.sentCommandBlocks.end() && matchesWireCommandBlock(record, known->second))
            return;
        auto wire = toWireCommandBlock(record);
        auto const cost = proto::estimatedEncodedSize(wire);
        if (!delta.commandBlocks.empty() && spent + cost > CommandBlockDeltaBytes)
        {
            backlog = CommandBlockBacklog::Pending;
            return;
        }
        spent += cost;
        follow.sentCommandBlocks.insert_or_assign(record.id.value, wire);
        delta.commandBlocks.push_back(std::move(wire));
    });

    // Everything this peer was told about that the host let go of: a prompt the next one discarded,
    // the oldest record the bound evicted, a reset. The mirror removes them; without this an
    // attached pane keeps a phantom block per empty Enter.
    std::erase_if(follow.sentCommandBlocks, [&](auto const& entry) {
        if (store.find(vtbackend::CommandBlockId { entry.first }) != nullptr)
            return false;
        delta.retiredCommandBlocks.push_back(entry.first);
        return true;
    });
    std::ranges::sort(delta.retiredCommandBlocks);

    // The gate closes only behind a COMPLETE walk; a budgeted one leaves it open for the remainder.
    follow.lastCommandBlockRevision = backlog == CommandBlockBacklog::Drained
                                          ? std::optional<uint64_t> { store.revision() }
                                          : std::nullopt;
    return backlog;
}
```

- [ ] **Step 5: Wire it into the push paths**

In `buildPush`, after `auto referencedLinks = …;` (line 572):

```cpp
    auto blockBacklog = CommandBlockBacklog::Drained;
```

after `collectLiveState(*terminal, follow, delta, state, session, std::to_underlying(screenType), mode);` (line 716), still inside the lock:

```cpp
        // After collectLiveState, which is what engages `state` on a snapshot.
        blockBacklog = collectCommandBlockState(*terminal, follow, delta, state, mode);
```

and the final return (line 745) becomes:

```cpp
    return BuiltPush { .delta = std::move(delta), .state = std::move(state), .blockBacklog = blockBacklog };
```

In `pushDelta`, replace the final `send(…)` (line 761) with:

```cpp
    auto const blockBacklog = built->blockBacklog;
    send(0, proto::DecodedPdu { std::move(built->delta) }, session.value);
    // Records the budget left behind go in the next debounce window, whether or not the session
    // produces more output — an idle shell after a flood raises no further screen update.
    if (blockBacklog == CommandBlockBacklog::Pending)
    {
        _pendingSessions.insert(session.value);
        scheduleFlush();
    }
```

In `streamSnapshots`, the body after `if (!built) continue;` (lines 818-822) becomes:

```cpp
        auto const blockBacklog = built->blockBacklog;
        // Always engaged on this path — a snapshot carries its session's whole state — but read as
        // the optional it is, so the invariant lives in ONE place rather than in every consumer.
        if (built->state)
            send(0, proto::DecodedPdu { *std::move(built->state) }, session.value);
        co_await sendSnapshotPieces(std::move(built->delta), session);
        // The snapshot stated only the newest records that fit; the older remainder follows as
        // increments, which the debounce delivers once this run is off the wire.
        if (blockBacklog == CommandBlockBacklog::Pending)
        {
            _pendingSessions.insert(session.value);
            scheduleFlush();
        }
```

- [ ] **Step 6: Run the tests**

Run: `cmake --build --preset clangcl-debug --target vthost_test` then `out/build/clangcl-debug/bin/vthost_test.exe "[native]"`
Expected: PASS — the five new cases and every existing `[native]` case. "a prompt the host discarded is retired on the client" depends on phase 1's §4.3 discard rule; a failure there is a contract question for the coordinator.

- [ ] **Step 7: Commit**

```bash
clang-format -i src/vthost/NativeSession.hpp src/vthost/NativeSession.cpp src/vthost/NativeSession_test.cpp
git add src/vthost/NativeSession.hpp src/vthost/NativeSession.cpp src/vthost/NativeSession_test.cpp
git commit -F - <<'EOF'
vthost: replicate command-block records per connection

A snapshot states the newest records that fit SnapshotChunkBytes; each
delta then carries, oldest first and within a 64 KiB budget, the
records that differ from the copy this connection was last sent, plus
the ids the host retired. The walk is gated on the store's revision,
exactly like the OSC 3008 context pool.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 2.8: Client side — `RemoteScreen` keeps the record table, stamped with revisions

The accumulated table is what makes the mirror robust to callbacks it never saw (a pane bound after the first deltas, a page flip's replay): like `contexts`, it always holds the latest state of every record the host still holds. Per-record revisions let the mirror adopt only what changed.

**Files:**
- Modify: `src/vthost/client/NativeClient.hpp:43` (new struct before `RemoteScreen`), `:93` (fields after `sessionEpoch`, Task 2.4)
- Modify: `src/vthost/client/NativeClient.cpp:63` (`apply(SessionState)`), `:158` (`apply(Delta)`, after the context-chain block)
- Test: `src/vthost/client/NativeClient_test.cpp` (append at end of file)

**Interfaces:**
- Consumes: Task 2.5's `SessionState::commandBlocks`, `Delta::commandBlocks`, `Delta::retiredCommandBlocks`.
- Produces (namespace `vthost::client`):
  - `struct MirroredCommandBlock { proto::WireCommandBlock wire; uint64_t revision = 0; bool operator==(MirroredCommandBlock const&) const = default; };`
  - `RemoteScreen::commandBlocks` (`std::map<uint32_t, MirroredCommandBlock>`, ordered: oldest id first), `RemoteScreen::commandBlocksRevision` (bumped per changed record), `RemoteScreen::commandBlocksRetiredAt` (the revision of the last removal).

- [ ] **Step 1: Write the failing tests**

Append to `src/vthost/client/NativeClient_test.cpp`:

```cpp
namespace
{
/// A wire record with host id @p id in raw state @p state.
proto::WireCommandBlock wireBlock(uint32_t id, uint8_t state)
{
    auto block = proto::WireCommandBlock {};
    block.id = id;
    block.state = state;
    block.commandLine = "cmd-" + std::to_string(id);
    return block;
}
} // namespace

TEST_CASE("RemoteScreen replaces its block table on a snapshot and stamps only what changed",
          "[vthost][attach][blocks]")
{
    auto screen = RemoteScreen {};
    auto first = proto::SessionState {};
    first.commandBlocks = { wireBlock(1, 2), wireBlock(2, 1) };
    screen.apply(first);
    REQUIRE(screen.commandBlocks.size() == 2);
    CHECK(screen.commandBlocksRetiredAt == 0); // nothing was there to remove
    auto const revisionOfTwo = screen.commandBlocks.at(2).revision;

    auto second = proto::SessionState {};
    second.commandBlocks = { wireBlock(2, 1), wireBlock(3, 0) };
    screen.apply(second);

    CHECK_FALSE(screen.commandBlocks.contains(1)); // the host no longer holds it ...
    CHECK(screen.commandBlocksRetiredAt > revisionOfTwo); // ... and the mirror is told to look
    CHECK(screen.commandBlocks.at(2).revision == revisionOfTwo); // unchanged: not news
    CHECK(screen.commandBlocks.at(3).revision > revisionOfTwo);  // new: news
    CHECK(screen.commandBlocksRevision >= screen.commandBlocks.at(3).revision);
}

TEST_CASE("RemoteScreen merges incremental blocks and erases retired ones", "[vthost][attach][blocks]")
{
    auto screen = RemoteScreen {};
    auto running = proto::Delta {};
    running.commandBlocks = { wireBlock(5, 1) };
    screen.apply(running);
    auto const first = screen.commandBlocks.at(5).revision;
    CHECK(first > 0);

    screen.apply(running); // the same record again: not news
    CHECK(screen.commandBlocks.at(5).revision == first);

    auto finished = proto::Delta {};
    finished.commandBlocks = { wireBlock(5, 2) };
    screen.apply(finished);
    CHECK(screen.commandBlocks.at(5).wire.state == 2);
    CHECK(screen.commandBlocks.at(5).revision > first);

    auto retire = proto::Delta {};
    retire.retiredCommandBlocks = { 5, 99 }; // 99 was never held: retiring it is a no-op
    screen.apply(retire);
    CHECK(screen.commandBlocks.empty());
    CHECK(screen.commandBlocksRetiredAt == screen.commandBlocksRevision);

    auto const quiet = screen.commandBlocksRevision;
    screen.apply(retire); // nothing left to retire: no revision spent
    CHECK(screen.commandBlocksRevision == quiet);
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset clangcl-debug --target vthost_test`
Expected: FAIL — does not compile (`'commandBlocks': is not a member of 'vthost::client::RemoteScreen'`).

- [ ] **Step 3: Declare the table**

In `src/vthost/client/NativeClient.hpp`, before `struct RemoteScreen` (line 43):

```cpp
/// One command-block record a mirror holds, and the RemoteScreen revision it last changed at — so a
/// consumer that remembers the revision it last synced at adopts only what moved since.
struct MirroredCommandBlock
{
    proto::WireCommandBlock wire; ///< The record as the host last sent it.
    uint64_t revision = 0;        ///< The RemoteScreen::commandBlocksRevision it last changed at.
    bool operator==(MirroredCommandBlock const&) const = default;
};
```

In `RemoteScreen`, after `int64_t sessionEpoch = 0;`:

```cpp
    /// The command-block records the host holds, keyed by the HOST's id and ordered by it (oldest
    /// first). A snapshot replaces the table; a delta merges into it and erases what it retires — so,
    /// like `contexts`, it is always the latest state, and a mirror that missed a callback catches up
    /// from it rather than from the deltas it never saw.
    std::map<uint32_t, MirroredCommandBlock> commandBlocks;
    /// Bumped once per record that changed (and stamped on it), and once per batch of removals.
    uint64_t commandBlocksRevision = 0;
    /// The revision of the last removal; a mirror synced before it has records to drop.
    uint64_t commandBlocksRetiredAt = 0;
```

- [ ] **Step 4: Maintain it**

In `src/vthost/client/NativeClient.cpp`, at the end of `RemoteScreen::apply(SessionState const&)` (after `sessionEpoch = state.sessionEpoch;`):

```cpp
    // REPLACED, not merged: a snapshot states every record the host holds (its newest, if capped —
    // the remainder follows as increments). Unchanged records keep their stamp, so a resync is not
    // mistaken for news; anything the snapshot no longer names is gone on the host too.
    auto replaced = std::map<uint32_t, MirroredCommandBlock> {};
    for (auto const& wire: state.commandBlocks)
    {
        auto const previous = commandBlocks.find(wire.id);
        auto const unchanged = previous != commandBlocks.end() && previous->second.wire == wire;
        replaced.insert_or_assign(
            wire.id,
            MirroredCommandBlock { .wire = wire,
                                   .revision = unchanged ? previous->second.revision : ++commandBlocksRevision });
    }
    if (std::ranges::any_of(commandBlocks, [&](auto const& entry) { return !replaced.contains(entry.first); }))
        commandBlocksRetiredAt = ++commandBlocksRevision;
    commandBlocks = std::move(replaced);
```

In `RemoteScreen::apply(Delta const&)`, after the `if (delta.contextChanged != 0) { … }` block (line 158):

```cpp
    for (auto const& wire: delta.commandBlocks)
    {
        if (auto const known = commandBlocks.find(wire.id); known != commandBlocks.end() && known->second.wire == wire)
            continue; // a re-send of what is already held: not news
        commandBlocks.insert_or_assign(wire.id,
                                       MirroredCommandBlock { .wire = wire, .revision = ++commandBlocksRevision });
    }
    auto retired = std::size_t { 0 };
    for (auto const id: delta.retiredCommandBlocks)
        retired += commandBlocks.erase(id);
    if (retired != 0)
        commandBlocksRetiredAt = ++commandBlocksRevision;
```

(`<algorithm>` is already included for `std::ranges::any_of`; `<map>` comes with the header.)

- [ ] **Step 5: Run the tests**

Run: `cmake --build --preset clangcl-debug --target vthost_test` then `out/build/clangcl-debug/bin/vthost_test.exe "[attach]"`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
clang-format -i src/vthost/client/NativeClient.hpp src/vthost/client/NativeClient.cpp src/vthost/client/NativeClient_test.cpp
git add src/vthost/client/NativeClient.hpp src/vthost/client/NativeClient.cpp src/vthost/client/NativeClient_test.cpp
git commit -F - <<'EOF'
vthost: keep the mirrored command-block table in RemoteScreen

A snapshot replaces the table, a delta merges into it and erases what it
retires, and every record carries the revision it last changed at, so a
mirror adopts only what moved since it last looked.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 2.9: Mirror side — `ScreenMirror` adopts records into the client terminal's store

Implements D4 and D5's client half.

**Files:**
- Modify: `src/vthost/client/ScreenMirror.hpp:189-194` (declarations after `syncContexts`), `:230` (member after `_contextIds`)
- Modify: `src/vthost/client/ScreenMirror.cpp:20` (include), `:178` (definitions after `syncContexts`), `:284` (`apply`), `:361`, `:371` (`fullReplay`)
- Test: `src/vthost/client/ScreenMirror_test.cpp:2-3` (includes), `:68-82` (`RecordingEvents`), cases appended at end of file

**Interfaces:**
- Consumes: Task 2.3 `Terminal::adoptCommandBlock`; C1 `Terminal::commandBlocks()`, `CommandBlockStore::find`, `forEachRecord`, `clear`; Task 2.6 `fromWireCommandBlock`, `matchesWireCommandBlock`; Task 2.8 `RemoteScreen::commandBlocks`, `commandBlocksRevision`, `commandBlocksRetiredAt`.
- Produces (private to `ScreenMirror`): `enum class CommandBlockSync : uint8_t { Follow = 0, Prime };`, `void syncCommandBlocks(RemoteScreen const&, CommandBlockSync);`, `void retireAbsentCommandBlocks(RemoteScreen const&);`, `uint64_t _syncedCommandBlocks = 0;`. Observable: the client terminal's `commandBlocks()` mirrors the host's records; `Events::commandBlockFinished` fires once per finish the client was attached for, never for one that predates its first replay.

- [ ] **Step 1: Write the failing tests**

In `src/vthost/client/ScreenMirror_test.cpp`, add after `#include <vtbackend/screen/Terminal.hpp>`:

```cpp
#include <vtbackend/screen/TerminalClocks.hpp>
#include <vtbackend/shell/CommandBlock.hpp>
```

`#include <utility>` among the standard includes (for `std::to_underlying`), and before `#include <vthost/GridWire.hpp>`:

```cpp
#include <vthost/CommandBlockWire.hpp>
```

Give `RecordingEvents` (lines 68-82) the finish events, after `std::string clipboard;`:

```cpp
    std::vector<vtbackend::CommandBlockSummary> finishedBlocks;
```

and after its `copyToClipboard` override:

```cpp
    void commandBlockFinished(vtbackend::CommandBlockSummary const& summary) override
    {
        finishedBlocks.push_back(summary);
    }
```

Append at the end of the file:

```cpp
namespace
{
constexpr auto PromptedAt = int64_t { 1'700'000'000'000'000'000 };

/// A Running record off the wire, as the host sends it between OSC 133;C and ;D.
proto::WireCommandBlock runningWire(uint32_t id)
{
    auto block = proto::WireCommandBlock {};
    block.id = id;
    block.state = std::to_underlying(vtbackend::CommandBlockState::Running);
    block.commandLine = "cmd-" + std::to_string(id);
    block.commandLineSource = std::to_underlying(vtbackend::CommandLineSource::Reported);
    block.present = proto::CommandBlockHasCommandStart;
    block.promptStartedAt = PromptedAt;
    block.commandStartedAt = PromptedAt + 1'000'000'000;
    return block;
}

/// @p id finished with @p exitCode, reported by OSC 133;D.
proto::WireCommandBlock finishedWire(uint32_t id, int exitCode)
{
    auto block = runningWire(id);
    block.state = std::to_underlying(vtbackend::CommandBlockState::Finished);
    block.end = std::to_underlying(vtbackend::CommandBlockEnd::Reported);
    block.present = proto::CommandBlockPresentMask;
    block.exitCode = exitCode;
    block.duration = 2'000'000'000;
    return block;
}

/// @p blocks as a snapshot's SessionState.
proto::SessionState stateWith(std::vector<proto::WireCommandBlock> blocks)
{
    auto state = proto::SessionState {};
    state.commandBlocks = std::move(blocks);
    return state;
}

/// A 5x3 mirror terminal whose events keep every command-block finish (RecordingEvents::finishedBlocks,
/// as MirrorHarness's do), the ScreenMirror bound to it and the RemoteScreen feeding it — the unit
/// harness for the record cases below.
struct BlockMirror
{
    RecordingEvents events;
    std::unique_ptr<vtbackend::Terminal> terminal;
    std::unique_ptr<ScreenMirror> mirror;
    vthost::client::RemoteScreen screen;

    BlockMirror()
    {
        auto settings = vtbackend::Settings {};
        settings.pageSize = vtbackend::PageSize { vtbackend::LineCount(3), vtbackend::ColumnCount(5) };
        settings.historyLimits = vtbackend::HistoryLimits::plain(vtbackend::LineCount(10));
        terminal = std::make_unique<vtbackend::Terminal>(events,
                                                         core::defaultEnvironment(),
                                                         std::make_unique<vtpty::MockPty>(settings.pageSize),
                                                         std::move(settings),
                                                         vtbackend::TerminalClocks::system(),
                                                         std::chrono::steady_clock::time_point {});
        mirror = std::make_unique<ScreenMirror>(*terminal);
    }

    /// Delivers @p state and a snapshot of @p rows (rows 10..12, blank, when empty) — an attach the
    /// first time, a resync after that.
    void snapshot(proto::SessionState state, std::vector<proto::WireLine> rows = {})
    {
        state.columns = 5;
        state.lines = 3;
        screen.apply(state);
        auto delta = proto::Delta {};
        delta.snapshot = 1;
        delta.stableViewportBase = 10;
        delta.stableFloor = 10;
        delta.lines = rows.empty() ? std::vector { rowAt(10), rowAt(11), rowAt(12) } : std::move(rows);
        screen.apply(delta);
        mirror->apply(screen, delta);
    }

    /// Delivers an increment carrying @p blocks and retiring @p retired, over an unmoved viewport.
    void increment(std::vector<proto::WireCommandBlock> blocks, std::vector<uint32_t> retired = {})
    {
        auto delta = proto::Delta {};
        delta.stableViewportBase = 10;
        delta.stableFloor = 10;
        delta.commandBlocks = std::move(blocks);
        delta.retiredCommandBlocks = std::move(retired);
        screen.apply(delta);
        mirror->apply(screen, delta);
    }

    /// @return The mirror's record for host id @p id, or nullptr.
    [[nodiscard]] vtbackend::CommandBlockRecord const* record(uint32_t id) const
    {
        return terminal->commandBlocks().find(vtbackend::CommandBlockId { id });
    }
};

/// @return The newest record's state in @p terminal's store, or nullopt when it holds none.
std::optional<vtbackend::CommandBlockState> newestState(vtbackend::Terminal const& terminal)
{
    auto state = std::optional<vtbackend::CommandBlockState> {};
    terminal.commandBlocks().forEachRecord(
        [&](vtbackend::CommandBlockRecord const& record) { state = record.state; });
    return state;
}

/// Whether @p mirror's store holds exactly @p server's records, over every field the wire carries.
bool storesMatch(vtbackend::Terminal const& server, vtbackend::Terminal const& mirror)
{
    auto matches = server.commandBlocks().size() == mirror.commandBlocks().size();
    server.commandBlocks().forEachRecord([&](vtbackend::CommandBlockRecord const& record) {
        auto const* const mirrored = mirror.commandBlocks().find(record.id);
        matches = matches && mirrored != nullptr
                  && vthost::matchesWireCommandBlock(*mirrored, vthost::toWireCommandBlock(record));
    });
    return matches;
}

/// Waits for the mirror to hold a Running block, has the host finish it, waits for the mirror to
/// see the finish, then detaches.
Task<void> finishWhileAttached(MirrorHarness* h, vtworkspace::SessionId session)
{
    co_await waitUntil(&h->loop,
                       [h] { return newestState(*h->mirror) == vtbackend::CommandBlockState::Running; });
    serverWrites(h, session, "\033]133;D;0\033\\");
    co_await waitUntil(&h->loop,
                       [h] { return newestState(*h->mirror) == vtbackend::CommandBlockState::Finished; });
    h->client->detach();
}
} // namespace

TEST_CASE("the attach snapshot's command blocks are adopted without finish events", "[vthost][mirror][blocks]")
{
    auto m = BlockMirror {};
    m.snapshot(stateWith({ finishedWire(1, 0), runningWire(2) }));

    REQUIRE(m.record(1) != nullptr);
    CHECK(m.record(1)->state == vtbackend::CommandBlockState::Finished);
    REQUIRE(m.record(2) != nullptr);
    CHECK(m.record(2)->state == vtbackend::CommandBlockState::Running);
    CHECK(m.record(2)->commandLine == "cmd-2");
    CHECK(m.events.finishedBlocks.empty()); // it finished before this client looked: history, not news
}

TEST_CASE("a finish the mirror watched start raises exactly one finish event", "[vthost][mirror][blocks]")
{
    auto m = BlockMirror {};
    m.snapshot(stateWith({ runningWire(2) }));
    m.increment({ finishedWire(2, 3) });

    REQUIRE(m.events.finishedBlocks.size() == 1);
    CHECK(m.events.finishedBlocks.front().id == vtbackend::CommandBlockId { 2 });
    CHECK(m.events.finishedBlocks.front().exitCode == 3);
    CHECK(m.events.finishedBlocks.front().commandLine == "cmd-2");

    m.increment({});                     // a delta that says nothing about blocks
    m.increment({ finishedWire(2, 3) }); // the same record re-sent
    CHECK(m.events.finishedBlocks.size() == 1);
}

TEST_CASE("a command that started and finished between two deltas still raises one", "[vthost][mirror][blocks]")
{
    auto m = BlockMirror {};
    m.snapshot(stateWith({ finishedWire(1, 0) }));
    m.increment({ finishedWire(2, 1) }); // never seen Running: it was quicker than one debounce window

    REQUIRE(m.events.finishedBlocks.size() == 1);
    CHECK(m.events.finishedBlocks.front().id == vtbackend::CommandBlockId { 2 });
}

TEST_CASE("an older record the mirror never held is adopted silently", "[vthost][mirror][blocks]")
{
    // The remainder of a capped snapshot, or a record this client's own bound evicted: history. A
    // reported finish only ever happens to the host's NEWEST record, so this cannot swallow news.
    auto m = BlockMirror {};
    m.snapshot(stateWith({ finishedWire(10, 0) }));
    m.increment({ finishedWire(4, 1) });

    CHECK(m.record(4) != nullptr);
    CHECK(m.events.finishedBlocks.empty());
}

TEST_CASE("a retired record leaves the mirror's store", "[vthost][mirror][blocks]")
{
    auto m = BlockMirror {};
    m.snapshot(stateWith({ finishedWire(1, 0), runningWire(2) }));
    m.increment({}, { 2 });

    CHECK(m.record(2) == nullptr);
    REQUIRE(m.record(1) != nullptr); // the rebuild kept everything else, as it was
    CHECK(m.record(1)->state == vtbackend::CommandBlockState::Finished);
    CHECK(m.events.finishedBlocks.empty()); // and announced nothing
}

TEST_CASE("a resync snapshot still reports a finish the mirror saw start", "[vthost][mirror][blocks]")
{
    // A resize resyncs a LIVE connection: this client was attached when the command ended, so its
    // finish is owed — unlike an attach, which is the mirror's first replay.
    auto m = BlockMirror {};
    m.snapshot(stateWith({ runningWire(4) }));
    m.snapshot(stateWith({ finishedWire(4, 0) }));

    CHECK(m.events.finishedBlocks.size() == 1);
}

TEST_CASE("reattaching never replays a finish that happened while detached", "[vthost][mirror][blocks]")
{
    // A reattach is a fresh NativeController and ScreenMirror (NativeController.hpp:350), so its
    // first replay is Prime: this first attach to an already-finished block IS the reattach case.
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    h.serverTerminal(session)->writeToScreen(
        "\033]133;A\033\\$ \033]133;C;cmdline_url=make\033\\\r\nbuilt\r\n\033]133;D;0\033\\");

    h.loop.blockOn(drive(&h, settleThenDetach(&h, [mirror = h.mirror.get()] {
                             return newestState(*mirror) == vtbackend::CommandBlockState::Finished;
                         })));

    REQUIRE(newestState(*h.mirror) == vtbackend::CommandBlockState::Finished);
    CHECK(h.mirrorEvents.finishedBlocks.empty());
    CHECK(storesMatch(*h.serverTerminal(session), *h.mirror));
}

TEST_CASE("a command finishing while attached raises exactly one finish on the client", "[vthost][mirror][blocks]")
{
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    h.serverTerminal(session)->writeToScreen("\033]133;A\033\\$ \033]133;C;cmdline_url=sleep%2010\033\\\r\n");

    h.loop.blockOn(drive(&h, finishWhileAttached(&h, session)));

    REQUIRE(h.mirrorEvents.finishedBlocks.size() == 1);
    CHECK(h.mirrorEvents.finishedBlocks.front().commandLine == "sleep 10");
    CHECK(h.mirrorEvents.finishedBlocks.front().exitCode == 0);
    CHECK(storesMatch(*h.serverTerminal(session), *h.mirror));
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset clangcl-debug --target vthost_test` then `out/build/clangcl-debug/bin/vthost_test.exe "[mirror][blocks]"`
Expected: FAIL — the record cases find no record (`m.record(1) == nullptr`), the closed-loop cases time out waiting for the mirror to hold a block.

- [ ] **Step 3: Declare the sync**

In `src/vthost/client/ScreenMirror.hpp`, after `void syncContexts(RemoteScreen const& screen);` (line 194):

```cpp
    /// Which rule a command-block sync adopts records under.
    enum class CommandBlockSync : uint8_t
    {
        Follow = 0, ///< The mirror has been following the session: a change it sees is news.
        Prime = 1,  ///< The mirror's first replay — the attach: everything it sees is history.
    };

    /// Brings the mirror terminal's command-block store in line with @p screen's table, ahead of the
    /// rows for the reason syncContexts() is.
    ///
    /// Adopts, oldest first, each record that changed since the last sync and differs from the
    /// store's copy. On @p sync Prime every record is AdoptMode::Snapshot: reattaching must never
    /// replay a finish that happened while detached. Otherwise a record is AdoptMode::Live — so a
    /// finish raises Events::commandBlockFinished once — unless the store never held it and it is
    /// older than the newest it holds, which is history this mirror was never shown.
    void syncCommandBlocks(RemoteScreen const& screen, CommandBlockSync sync);

    /// Drops from the store every record @p screen's table no longer holds (the host retired it).
    ///
    /// Through the C1 surface alone — copy the survivors, `clear()`, re-adopt them as Snapshot — so it
    /// costs one pass over the store, and runs only when a sync finds a removal it has not seen.
    void retireAbsentCommandBlocks(RemoteScreen const& screen);
```

and after `vthost::ContextIdMap _contextIds;` (line 230):

```cpp
    /// The RemoteScreen::commandBlocksRevision this mirror last synced at.
    uint64_t _syncedCommandBlocks = 0;
```

- [ ] **Step 4: Implement it**

In `src/vthost/client/ScreenMirror.cpp`, add `#include <vthost/CommandBlockWire.hpp>` before `#include <vthost/ContextWire.hpp>`, and after `syncContexts` (line 178):

```cpp
void ScreenMirror::syncCommandBlocks(RemoteScreen const& screen, CommandBlockSync sync)
{
    // Gated like the host's walk: nothing in the table moved since the last sync, nothing to adopt.
    // The first replay always runs — it has never synced.
    if (sync == CommandBlockSync::Follow && screen.commandBlocksRevision == _syncedCommandBlocks)
        return;
    if (sync == CommandBlockSync::Prime || screen.commandBlocksRetiredAt > _syncedCommandBlocks)
        retireAbsentCommandBlocks(screen);

    auto& store = _terminal->commandBlocks();
    // The newest id held before this sync, advanced as records are adopted oldest first. A reported
    // finish only ever happens to the host's newest record (OSC 133;D finishes the current one), so a
    // record this store never held that is OLDER than what it holds cannot be news.
    auto newest = uint32_t { 0 };
    store.forEachRecord(
        [&](vtbackend::CommandBlockRecord const& record) { newest = std::max(newest, record.id.value); });

    for (auto const& [id, mirrored]: screen.commandBlocks)
    {
        if (sync == CommandBlockSync::Follow && mirrored.revision <= _syncedCommandBlocks)
            continue;
        auto const* const held = store.find(vtbackend::CommandBlockId { id });
        if (held != nullptr && matchesWireCommandBlock(*held, mirrored.wire))
            continue;
        auto const mode = (sync == CommandBlockSync::Prime || (held == nullptr && id < newest))
                              ? vtbackend::AdoptMode::Snapshot
                              : vtbackend::AdoptMode::Live;
        // `held` is read before the adoption, which may move the store's records.
        _terminal->adoptCommandBlock(fromWireCommandBlock(mirrored.wire, held), mode);
        newest = std::max(newest, id);
    }
    _syncedCommandBlocks = screen.commandBlocksRevision;
}

void ScreenMirror::retireAbsentCommandBlocks(RemoteScreen const& screen)
{
    auto& store = _terminal->commandBlocks();
    auto kept = std::vector<vtbackend::CommandBlockRecord> {};
    auto stale = std::size_t { 0 };
    store.forEachRecord([&](vtbackend::CommandBlockRecord const& record) {
        if (screen.commandBlocks.contains(record.id.value))
            kept.push_back(record);
        else
            ++stale;
    });
    // At default settings the client's own bound already evicted what the host's bound evicted, so
    // this is a scan that finds nothing; the rebuild below runs for discarded prompts and resets.
    if (stale == 0)
        return;
    store.clear();
    for (auto& record: kept)
        _terminal->adoptCommandBlock(std::move(record), vtbackend::AdoptMode::Snapshot);
}
```

In `apply`, inside the lock after `syncContexts(screen);` (line 284):

```cpp
        syncCommandBlocks(screen, CommandBlockSync::Follow); // and before any row: a head row needs its record
```

In `fullReplay`, replace `_primed = true;` (line 361) with:

```cpp
    // Decided before `_primed` flips: the first replay is the attach, and nothing it shows is news.
    auto const blockSync = _primed ? CommandBlockSync::Follow : CommandBlockSync::Prime;
    _primed = true;
```

and inside its lock after `syncContexts(screen);` (line 371):

```cpp
        syncCommandBlocks(screen, blockSync); // and before any row: a head row needs its record
```

- [ ] **Step 5: Run the tests**

Run: `cmake --build --preset clangcl-debug --target vthost_test` then `out/build/clangcl-debug/bin/vthost_test.exe "[vthost]"`
Expected: PASS — the eight new cases and every existing case.

- [ ] **Step 6: Commit**

```bash
clang-format -i src/vthost/client/ScreenMirror.hpp src/vthost/client/ScreenMirror.cpp src/vthost/client/ScreenMirror_test.cpp
git add src/vthost/client/ScreenMirror.hpp src/vthost/client/ScreenMirror.cpp src/vthost/client/ScreenMirror_test.cpp
git commit -F - <<'EOF'
vthost: adopt mirrored command-block records into the client terminal

The mirror's first replay adopts every record as a snapshot, so a
reattach never replays a finish that happened while detached; after
that a change is adopted live, so a finish the client watched raises
commandBlockFinished exactly once. Records the host retired leave the
client's store.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 2.10: Mirror side — head positions in the mirror's own row ids

Implements D3's client half: the scrollbar marks (phase 8) and the sticky header (phase 7) read `headStableId`, and on a client it must name the client's row.

**Files:**
- Modify: `src/vthost/client/ScreenMirror.hpp:155-161` (declaration after `writeRow`)
- Modify: `src/vthost/client/ScreenMirror.cpp:180-210` (`writeRow`), definition after `scrollInRow` (:218), `fullReplay` (:396-400, after the Discard `clearHistory` block)
- Test: `src/vthost/client/ScreenMirror_test.cpp` (append at end of file)

**Interfaces:**
- Consumes: C1 `CommandBlockStore::find`, `forEachRecord`, `updateHeadPosition(CommandBlockId, int64_t, uint64_t)`, `LineFlag::Marked`; `Grid::stableLineIdOf`, `Grid::stableIdGeneration`; Task 2.6 `UnknownHeadGeneration`; Task 2.9's sync running before rows.
- Produces (private): `void ScreenMirror::trackHead(vtbackend::Grid const& grid, vtbackend::LineOffset offset, proto::WireLine const& row);`. Observable: a mirrored record's `headStableId`/`headIdGeneration` name the mirror's own row carrying `LineFlag::Marked` and that block id, once that row has been written; until then the generation is `UnknownHeadGeneration`. A full replay that rebuilds history (`clearHistory()` keeps the stable-id generation while the rows are re-streamed under new ids) forgets every head first, so the rows it streams re-anchor them.

- [ ] **Step 1: Write the failing tests**

Append to `src/vthost/client/ScreenMirror_test.cpp`:

```cpp
TEST_CASE("the mirror records a block's head where it wrote the head row", "[vthost][mirror][blocks]")
{
    auto m = BlockMirror {};
    auto head = rowAt(10, "$ ls");
    head.blockId = 7;
    head.flags = static_cast<uint16_t>(vtbackend::LineFlag::Marked);
    auto output = rowAt(11, "a");
    output.blockId = 7;
    m.snapshot(stateWith({ finishedWire(7, 0), runningWire(8) }), { head, output, rowAt(12) });

    auto const& grid = m.terminal->primaryScreen().grid();
    REQUIRE(m.record(7) != nullptr);
    CHECK(m.record(7)->headStableId == grid.stableLineIdOf(vtbackend::LineOffset(0)));
    CHECK(m.record(7)->headIdGeneration == grid.stableIdGeneration());
    // Block 8's head row never arrived: its head is unknown — never a row id from the host's space.
    REQUIRE(m.record(8) != nullptr);
    CHECK(m.record(8)->headIdGeneration == vthost::UnknownHeadGeneration);
}

TEST_CASE("a mirrored record's head resolves to the mirror's own head row", "[vthost][mirror][blocks]")
{
    auto h = MirrorHarness {};
    h.host.createTab();
    auto const session = h.host.model().window(h.host.windowId())->activeTab()->rootPane()->session();
    h.serverTerminal(session)->writeToScreen(
        "\033]133;A\033\\$ \033]133;C;cmdline_url=ls\033\\\r\nfile\r\n\033]133;D;0\033\\");

    h.loop.blockOn(drive(&h, settleThenDetach(&h, [mirror = h.mirror.get()] {
                             return newestState(*mirror) == vtbackend::CommandBlockState::Finished;
                         })));

    auto id = vtbackend::CommandBlockId {};
    h.mirror->commandBlocks().forEachRecord([&](vtbackend::CommandBlockRecord const& record) { id = record.id; });
    auto const* const record = h.mirror->commandBlocks().find(id);
    REQUIRE(record != nullptr);
    auto const& grid = h.mirror->primaryScreen().grid();
    REQUIRE(record->headIdGeneration == grid.stableIdGeneration());

    auto heads = 0;
    for (auto const row: std::views::iota(0, unbox<int>(grid.pageSize().lines)))
    {
        if (grid.stableLineIdOf(vtbackend::LineOffset(row)) != record->headStableId)
            continue;
        auto const& line = grid.lineAt(vtbackend::LineOffset(row));
        CHECK(line.blockId() == record->id);
        CHECK(line.flags().contains(vtbackend::LineFlag::Marked));
        ++heads;
    }
    CHECK(heads == 1);
}

TEST_CASE("a Discard replay re-anchors heads", "[vthost][mirror][blocks]")
{
    // A Discard replay that rebuilds history re-streams every row under ids the mirror's grid mints
    // anew, while its stable-id generation stays put (Grid::clearHistory() is deliberately no bump):
    // trackHead()'s first-wins guard alone would keep the head the first replay recorded.
    auto m = BlockMirror {};
    auto head = rowAt(10, "$ ls");
    head.blockId = 7;
    head.flags = static_cast<uint16_t>(vtbackend::LineFlag::Marked);
    m.snapshot(stateWith({ finishedWire(7, 0) }), { head, rowAt(11), rowAt(12) });

    auto const& grid = m.terminal->primaryScreen().grid();
    REQUIRE(m.record(7) != nullptr);
    auto const firstHead = m.record(7)->headStableId;
    REQUIRE(firstHead == grid.stableLineIdOf(vtbackend::LineOffset(0)));

    // A host-side reset: a generation bump at an unchanged size is a Discard replay. Three rows of
    // history above the page are streamed through it first, so the head row (host id 10) scrolls in
    // at the bottom under a newly minted mirror id, and the id the first replay recorded ends up on
    // the blank history row 7.
    auto rebuilt = proto::Delta {};
    rebuilt.snapshot = 1;
    rebuilt.generation = 1;
    rebuilt.stableViewportBase = 10;
    rebuilt.stableFloor = 7;
    rebuilt.lines = { rowAt(7), rowAt(8), rowAt(9), head, rowAt(11), rowAt(12) };
    m.screen.apply(rebuilt);
    m.mirror->apply(m.screen, rebuilt);
    REQUIRE(grid.historyLineCount() == vtbackend::LineCount(3));

    REQUIRE(m.record(7) != nullptr);
    CHECK(m.record(7)->headStableId != firstHead);
    CHECK(m.record(7)->headStableId == grid.stableLineIdOf(vtbackend::LineOffset(0))); // row 10, page top
    CHECK(m.record(7)->headIdGeneration == grid.stableIdGeneration());
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset clangcl-debug --target vthost_test` then `out/build/clangcl-debug/bin/vthost_test.exe "[mirror][blocks]"`
Expected: FAIL — record 7's `headIdGeneration` is `UnknownHeadGeneration`; the closed-loop case fails its `REQUIRE` on the generation; the Discard case fails its first `REQUIRE` on the head the same way.

- [ ] **Step 3: Declare it**

In `src/vthost/client/ScreenMirror.hpp`, after the `writeRow` declaration (line 161):

```cpp
    /// Records @p row as its block's head when it carries LineFlag::Marked and the block id, and the
    /// block has no head in @p grid's current stable-id generation yet.
    ///
    /// The host's cached head is a row id in the HOST's grid and never crosses the wire (@see
    /// proto::WireCommandBlock); this is where the mirror learns its own. First one wins within a
    /// generation, as on the host, which records the head once at OSC 133;A — and a full replay
    /// streams rows top-down, so "first" is the block's topmost Marked row. A replay that rebuilds
    /// history forgets every head before it streams (@see fullReplay()): the generation survives that
    /// rebuild, the row ids do not.
    /// @param grid The grid @p row was just written into.
    /// @param offset Where in @p grid.
    /// @param row The wire row written there.
    void trackHead(vtbackend::Grid const& grid, vtbackend::LineOffset offset, proto::WireLine const& row);
```

- [ ] **Step 4: Implement it and call it from `writeRow`**

In `src/vthost/client/ScreenMirror.cpp`, `writeRow`'s scrollback branch (line 198) becomes:

```cpp
        applyWireLine(page.grid().changingLineAt(offset), *row, _linkIds, _contextIds);
        trackHead(page.grid(), offset, *row);
        return;
```

and its tail (lines 204-209) becomes:

```cpp
    if (row != nullptr)
    {
        applyWireLine(line, *row, _linkIds, _contextIds);
        trackHead(page.grid(), offset, *row);
    }
    else
    {
        // A row the mirror has no data for is cleared rather than left holding whatever was
        // there before: stale content is worse than a blank line, because it looks correct.
        line.reset(vtbackend::LineFlags {}, vtbackend::GraphicsAttributes {});
    }
```

After `scrollInRow` (line 218):

```cpp
void ScreenMirror::trackHead(vtbackend::Grid const& grid, vtbackend::LineOffset offset, proto::WireLine const& row)
{
    if (row.blockId == 0 || !vtbackend::LineFlags::fromValue(row.flags).contains(vtbackend::LineFlag::Marked))
        return;
    auto& store = _terminal->commandBlocks();
    auto const id = vtbackend::CommandBlockId { row.blockId };
    auto const* const record = store.find(id);
    // An id with no record (the host's bound dropped it) has nothing to anchor; a head already known
    // in this generation stays — and a write that changes nothing would only bump the store's revision.
    if (record == nullptr || record->headIdGeneration == grid.stableIdGeneration())
        return;
    store.updateHeadPosition(id, grid.stableLineIdOf(offset), grid.stableIdGeneration());
}
```

In `fullReplay`, directly after the Discard block

```cpp
        if (history == LocalHistory::Discard && !wantAlternate)
        {
            page.grid().clearHistory();
            _terminal->scrollbackBufferCleared();
        }
```

insert:

```cpp
        if (rebuildHistory)
        {
            // The rows below are re-streamed under ids this grid mints anew, but its stable-id
            // generation stays put (clearHistory() is deliberately no bump), so trackHead()'s
            // first-wins guard would keep every head on a row that now holds something else.
            // Forget them all; the head rows streamed below record them afresh.
            auto& store = _terminal->commandBlocks();
            store.forEachRecord([&](vtbackend::CommandBlockRecord const& record) {
                store.updateHeadPosition(record.id, 0, UnknownHeadGeneration);
            });
        }
```

(`forEachRecord`'s visitor may call `updateHeadPosition()`, phase 1 Task 1.3. Without this block the Discard case fails: its head still names the id now on history row 7.)

- [ ] **Step 5: Run the tests**

Run: `cmake --build --preset clangcl-debug --target vthost_test` then `out/build/clangcl-debug/bin/vthost_test.exe "[vthost]"`
Expected: PASS — the three new cases and every existing case.

- [ ] **Step 6: Commit**

```bash
clang-format -i src/vthost/client/ScreenMirror.hpp src/vthost/client/ScreenMirror.cpp src/vthost/client/ScreenMirror_test.cpp
git add src/vthost/client/ScreenMirror.hpp src/vthost/client/ScreenMirror.cpp src/vthost/client/ScreenMirror_test.cpp
git commit -F - <<'EOF'
vthost: anchor mirrored blocks on the mirror's own head rows

A record's cached head names a row in the host's grid, so it never
crosses the wire. The mirror sets it when it writes the block's Marked
row, in its own stable-id space; until then the head is unknown. A
replay that rebuilds history forgets every head first: the rows come
back under new ids while the stable-id generation stays the same.

Signed-off-by: Christian Parpart <christian@parpart.family>
EOF
```

### Task 2.11: Phase gate

**Files:** whatever the steps below change; nothing else.

**Interfaces:** none new.

- [ ] **Step 1: Full build, zero warnings**

Run: `cmake --build --preset clangcl-debug`
Expected: success with no warnings. `Terminal.hpp` changed, so every module rebuilds, the GUI's `NativeController` (a `ScreenMirror` user) included. If `src/contour/display/*` fails on `yaml-cpp/emitter.h`, that break is pre-existing on this machine (README); build `vtbackend_test vthost_test vtworkspace_test contour_test contour_gui_test` individually and compile `src/contour/remote/NativeController.cpp` alone via `ninja -t commands` + `cmd /c`.

- [ ] **Step 2: Full suite and spelling**

Run: `ctest --test-dir out/build/clangcl-debug --output-on-failure` and `ctest --test-dir out/build/clangcl-debug -L daemon --output-on-failure` and `ctest --test-dir out/build/clangcl-debug -R check_spelling --output-on-failure`
Expected: every test that passed at phase 0's baseline passes; the `daemon` label is fully green; spelling PASS (or SKIP when `typos` is absent). Record the pass count for Step 6.

- [ ] **Step 3: Hand-audit what clang-tidy cannot lint here**

`git diff <phase-2-start>..HEAD -- src/` and check every added line for: `misc-const-correctness` (locals that can be `const`), `misc-use-internal-linkage` (test helpers outside an anonymous namespace), `bugprone-implicit-widening-of-multiplication-result`, `readability-identifier-naming` (`_member` only on non-public members), no `NOLINT`, no new `bool` parameter/return/member outside the documented predicates, every header `#pragma once`. Fix and re-run Step 2 if anything changed.

- [ ] **Step 4: `/simplify`**

Run `/simplify` over `git diff <phase-2-start>..HEAD`. Likely candidates: the two `*Wire.hpp` enum validators against `ContextWire.hpp`'s, and the test helpers duplicated between `NativeSession_test.cpp` and `ScreenMirror_test.cpp`. Commit its fixes (`vthost: simplify the command-block replication`), then repeat Step 2.

- [ ] **Step 5: Code review at xhigh**

Run `/code-review xhigh` on the phase's commits (or dispatch a review subagent with `effort: "xhigh"`). Brief the reviewer with D1–D6 and these probes: a record changing while a budgeted walk is pending; a snapshot superseding a flood mid-drain; `dropTagged` discarding a delta that carried records or retirements; the mirror's first replay arriving after several deltas (`NativeController::primeBinding`); a host RIS (`clear()` and a generation bump in one batch); the client's `maxRecords` smaller than the host's; a hostile peer sending out-of-range enum bytes, `bornAt` above 24 bits, or a 10 MB command line. Fix every confirmed finding with a test; commit each as `vthost: <fix>`; repeat Step 2.

- [ ] **Step 6: Record the gate**

The phase's last commit (Step 5's last fix, else Step 4's) carries in its body, above the
`Signed-off-by` line, the ctest summary line measured in the final Step 2 run verbatim (e.g.
`ctest: 100% tests passed, 0 tests failed out of 12`) plus the names of any baseline failures. If
neither step produced a commit, do not make an empty one (phase 0's rule): report that line to the
coordinator instead. The coordinating session — never a subagent — then records progress.

