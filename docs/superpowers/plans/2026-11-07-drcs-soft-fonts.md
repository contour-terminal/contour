# DECDLD Soft Fonts, Finished — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Real-world DECDLD soft fonts load correctly, are sized for the cell they were designed for, render as
crisp monochrome masks coloured from each cell's real attributes, keep the underlying characters for
copy and search, and work in daemon-hosted sessions and across a live upgrade.

**Architecture:** A pure, table-driven font model in `vtbackend/vt/` (decoding, sixel accumulation, target-cell
detection ported from Windows Terminal, error glyph) is owned by `Terminal` as a copy-on-write `SoftFontStore`.
A grid cell stores its real codepoint plus a `SoftGlyphRef {font, position}` in a sparse per-line side table.
`RenderBufferBuilder` resolves that reference to an immutable `shared_ptr<SoftGlyph const>` under the state
lock. A new `SoftFontRenderer` in `vtrasterizer`, a sibling of `BoxDrawingRenderer`, uploads nearest-neighbour
scaled alpha tiles to the shared atlas. The daemon mirrors the store with one capability-gated PDU and the
references with an appended `Delta` field.

**Tech Stack:** C++23, Catch2, Qt6 (offscreen verification only), CMake presets.

**Spec:** `docs/superpowers/specs/2026-11-07-drcs-soft-fonts-design.md`. Read it first. It holds every
ruling this plan relies on.

**Primary references:**
- Windows Terminal `src/terminal/adapter/FontBuffer.cpp`, `FontBuffer.hpp`, `DispatchTypes.hpp` and
  `terminalOutput.cpp`, pinned at commit `cbc857bdf7749981890c5b561ee44ea31948bfb1`
  (`https://raw.githubusercontent.com/microsoft/terminal/<sha>/src/terminal/adapter/FontBuffer.cpp`).
- The VT510 DECDLD page: https://vt100.net/docs/vt510-rm/DECDLD.html

## Global Constraints

- Follow `AGENT.md`: C++23, no C-style loops, `[[nodiscard]]`, Doxygen on new public API, `.hpp` with
  `#pragma once`, `enum class` with an explicit `: uint8_t` and the zero value as the off/default case, no
  `bool` parameters, configuration at construction time, `std::expected` for fallible decoding.
- Zero warnings under `-Werror`, with clang-tidy clean. No `NOLINT`. clang-format all C++ files, but never
  `.qml` files.
- `vtbackend` layering: `core/` includes only `core/`, and `vt/` includes nothing above `vt/`. `vthost`
  links no Qt.
- **Never edit `vendor/core-cpp`.**
- Build with `cmake --build --preset clang-asan` (on macOS the existing tree is `out/appleclang-debug`, so use
  whichever preset is configured). Run tests with `ctest --preset=clang-asan`.
- Crisp scaling only: nearest-neighbour, filling the cell exactly on each axis. No filtering.
- `bench-headless` instruction counts on `ascii`, `utf8` and `tui` must not move (spec anti-goal).
- **Do not commit third-party font data** (the vt100.net fonts carry no stated licence, and cmatrix is
  GPL-3). Test fixtures are *synthetic* sequences that reproduce each real font's header and geometry. The
  real files are used only in the Task 8 visual check, from a scratch directory.
- Commits end with `Signed-off-by: Christian Parpart <christian@parpart.family>`, with no AI trailers.

## Review Focus

1. **Reflow and resize with soft-glyph cells in scrollback.** The side table must move with its cells
   exactly as `imageFragments` does (`LineSoA.cpp` copy at ~204). Covered by a test in Task 2.
2. **Overwriting a soft-glyph cell with ordinary text**, including the bulk ASCII path and erase
   (ED/EL/ECH), must drop its reference. Otherwise a stale glyph draws over new text. Covered by tests in
   Tasks 2 and 3.
3. **ICH/DCH and scrolling regions shifting cells sideways** carry the reference with the cell. Covered by a
   test in Task 2.
4. **Copying a selection across mixed soft and ordinary cells** yields the underlying characters. Covered by
   a test in Task 3.
5. **Font-size change while soft glyphs are on screen, including DECDWL/DECDHL rows.** The tiles must
   regenerate at the new cell size, and the halves must be correct. Covered by a test in Task 5.

Known and out of scope: larsbrinkhoff's `twit.txt`/`ictech.txt` rely on VT220 sixel-column overflow into the
next character. Windows Terminal does not support this either.

---

## File Structure

| File | Responsibility |
|------|----------------|
| `src/vtbackend/core/SoftGlyphRef.hpp` (new) | `SoftGlyphRef {font, position}`: the per-cell reference. It sits in `core/` because `grid/`, `vt/`, `state/` and `vthost` all name it. |
| `src/vtbackend/vt/SoftFont.hpp/.cpp` (new) | Parameter decoding, `SoftGlyph`, `SoftFont`, target-cell detection table, sixel accumulation, packing and centring, error glyph. Pure. |
| `src/vtbackend/vt/SoftFontStore.hpp/.cpp` (new) | Copy-on-write fonts by number, designator lookup, erase semantics, resolution, generation counter. Pure. |
| `src/vtbackend/vt/SoftFont_test.cpp`, `SoftFontStore_test.cpp` (new) | Table-driven unit tests. |
| `src/vtbackend/grid/LineSoA.hpp/.cpp`, `CellProxy.hpp`, `Line.cpp`, `SoAClusterWriter.*` | The `softGlyphs` side table, mirroring every `imageFragments` site. |
| `src/vtbackend/screen/Terminal.hpp/.cpp`, `Screen.hpp/.cpp` | DECDLD dispatch, SCS precedence, write path. `DRCSGlyph`, `DRCSCharset`, `defineDRCS` and `createDRCSImage` are removed. |
| `src/vtbackend/render/RenderBuffer.hpp`, `screen/RenderBufferBuilder.cpp` | `RenderCell::softGlyph`, resolved under the lock. |
| `src/vtrasterizer/SoftFontRenderer.hpp/.cpp` (new), `TextRenderer.*`, `TextClusterGrouper.*` | Mask tiles, scaling, dispatch. |
| `src/vtbackend/state/TerminalState.*`, `GridState.cpp` | Persistence. |
| `src/vthost/proto/Negotiation.hpp`, `Pdu.hpp/.cpp`, `NativeSession.cpp`, `GridWire.*`, `client/ScreenMirror.*`, `testing/GridParity.cpp` | Daemon mirroring. |

---

### Task 1: The soft font model (pure)

**Files:**
- Create: `src/vtbackend/core/SoftGlyphRef.hpp`, `src/vtbackend/vt/SoftFont.hpp`, `src/vtbackend/vt/SoftFont.cpp`,
  `src/vtbackend/vt/SoftFontStore.hpp`, `src/vtbackend/vt/SoftFontStore.cpp`,
  `src/vtbackend/vt/SoftFont_test.cpp`, `src/vtbackend/vt/SoftFontStore_test.cpp`
- Modify: `src/vtbackend/CMakeLists.txt` (sources and test sources)

**Interfaces (Produces):**

```cpp
// core/SoftGlyphRef.hpp
struct SoftGlyphRef
{
    uint8_t font = 0;     ///< Pfn, 0..2.
    uint8_t position = 0; ///< 7-bit position in the set, 0x20..0x7F.
    auto operator<=>(SoftGlyphRef const&) const = default;
};

// vt/SoftFont.hpp
enum class SoftFontUsage : uint8_t { Text, FullCell };
enum class SoftCharsetSize : uint8_t { Size94, Size96 };
enum class SoftFontErase : uint8_t { AllChars = 0, ReloadedChars = 1, AllRenditions = 2 };
enum class SoftFontError : uint8_t { FontNumber, EraseControl, CellMatrix, FontSet, Usage, CharsetSize, TooLarge, Designator };

struct DecdldParameters { int pfn, pcn, pe, pcmw, pss, pt, pcmh, pcss; };  ///< raw, absent = 0

enum class SoftFontSizeSource : uint8_t { Pixels, Matrix };  ///< Matrix = Pcmw 2/3/4

struct SoftFontDeclaration      ///< validated
{
    uint8_t fontNumber; uint8_t startPosition; SoftFontErase erase;
    uint8_t declaredWidth; uint8_t declaredHeight;     ///< 0 = not declared
    SoftFontSizeSource sizeSource;
    uint16_t columnsPerPage;                           ///< 80 or 132
    uint8_t linesPerPage;                              ///< 24, 36 or 48
    SoftFontUsage usage; SoftCharsetSize charsetSize;
    auto operator<=>(SoftFontDeclaration const&) const = default;
};

[[nodiscard]] std::expected<SoftFontDeclaration, SoftFontError> decodeDecdld(DecdldParameters const&);

struct SoftCellGeometry { uint8_t fullWidth; uint8_t fullHeight; uint8_t textWidth; }; ///< textWidth 0 = unclipped
[[nodiscard]] SoftCellGeometry detectTargetCell(SoftFontDeclaration const&, uint8_t usedWidth, uint8_t usedHeight);

struct SoftGlyph
{
    uint64_t id = 0;                ///< content hash of (width, height, coverage); the atlas key
    uint8_t width = 0, height = 0;  ///< == the font's SoftCellGeometry full size
    std::vector<uint8_t> coverage;  ///< row-major, top row first, 0 or 1
};

struct SoftFont
{
    SoftFontDeclaration declaration;
    std::string designator;                                   ///< Dscs: 0-2 intermediates + final
    SoftCellGeometry geometry;
    std::array<std::shared_ptr<SoftGlyph const>, 96> glyphs;  ///< index = position - 0x20; null = undefined
    std::shared_ptr<SoftGlyph const> errorGlyph;              ///< reverse '?', at geometry size
};

/// Makes a glyph and stamps its content-hash id. The one factory the decoder, the state codec
/// and the daemon mirror all use, so identity never needs persisting or sending.
[[nodiscard]] std::shared_ptr<SoftGlyph const> makeSoftGlyph(uint8_t width, uint8_t height, std::vector<uint8_t> coverage);

/// Assembles a font from already-built glyphs (codec and mirror path; no sixel decoding).
[[nodiscard]] SoftFont makeSoftFont(SoftFontDeclaration const&, std::string designator,
                                    std::array<std::shared_ptr<SoftGlyph const>, 96> glyphs);

/// Builds a font from a DECDLD body (Dscs + sixel data). @p previous is the font
/// currently under this number (for Pe=1 merging), or null.
[[nodiscard]] std::expected<SoftFont, SoftFontError> buildSoftFont(SoftFontDeclaration const&,
                                                                   std::string_view body,
                                                                   SoftFont const* previous);

// vt/SoftFontStore.hpp
enum class SoftGlyphLookup : uint8_t { FontGone, Undefined, Defined };
struct SoftGlyphResolution { SoftGlyphLookup lookup; std::shared_ptr<SoftGlyph const> glyph; }; ///< glyph = error glyph when Undefined

class SoftFontStore
{
  public:
    /// Applies one DECDLD. An invalid sequence leaves the store unchanged and reports why.
    [[nodiscard]] std::expected<void, SoftFontError> define(DecdldParameters const&, std::string_view body);
    void clear() noexcept;                       ///< RIS
    [[nodiscard]] std::shared_ptr<SoftFont const> font(uint8_t number) const noexcept;
    [[nodiscard]] std::optional<uint8_t> fontForDesignator(std::string_view dscs) const noexcept; ///< most recently loaded wins
    /// An out-of-range ref (font > 2, position outside 0x20..0x7F) resolves as FontGone.
    [[nodiscard]] SoftGlyphResolution resolve(SoftGlyphRef) const noexcept;
    [[nodiscard]] uint64_t generation() const noexcept;  ///< bumps on every change
    /// Replaces every font, used by the daemon mirror and the state codec.
    void replaceAll(std::array<std::shared_ptr<SoftFont const>, 3> fonts);
    [[nodiscard]] std::array<std::shared_ptr<SoftFont const>, 3> const& fonts() const noexcept;
  private: ...
};
```

**Semantics to implement (each pinned by a test below):**

- `decodeDecdld`:
  - `Pfn` must be 0..2.
  - `Pe` must be 0..2.
  - `Pcmw`: 0 means default, 1 is an error, 2/3/4 are the 5×10, 6×10 and 7×10 matrices
    (`SoftFontSizeSource::Matrix`), and 5..16 is a pixel width. A matrix requires usage Text, except 6×10
    with 132 columns.
  - `Pss`: 0/1 = 80×24, 2 = 132×24, 11 = 80×36, 12 = 132×36, 21 = 80×48, 22 = 132×48. Anything else is an
    error.
  - `Pt`: 0/1 = Text, 2 = FullCell.
  - `Pcmh` must be ≤ 32.
  - `Pcss`: 0 = 94-character set, 1 = 96-character set.
  - `Pcn`: in a 94-set an absent value means 1, in a 96-set it means 0. Start position = 0x20 + Pcn, which must
    be ≤ 0x7F.
- `detectTargetCell`: port `FontBuffer::_calculateDimensions` as a **data table**, one row per known terminal.
  Do not copy its `if` ladder.

  | cols | candidate (w×h) | result (full w, full h, text w) | only when no size declared |
  |------|-----------------|---------------------------------|------|
  | 80  | 8×10  | 10, 10, 8  | yes |
  | 80  | 15×12 | 15, 12, tw | no |
  | 80  | 10×16 | 10, 16, tw | no |
  | 80  | 10×20 | 10, 20, tw | no |
  | 80  | 12×30 | 12, 30, tw | no |
  | 132 | 6×10  | 6, 10, 0   | yes |
  | 132 | 9×12  | 9, 12, tw  | no |
  | 132 | 6×16  | 6, 16, tw  | no |
  | 132 | 6×20  | 6, 20, tw  | no |
  | 132 | 7×30  | 7, 30, tw  | no |

  - Fallback: 16, 32, tw.
  - `tw` = declared width for Text, 0 for FullCell.
  - The table is preceded by three overrides:
    - Matrix → (6, 10, 0) if 132 columns and width ≤ 6, else (10, 10, 8).
    - Declared width and height on a FullCell font → (w, h, 0).
    - Lines ≠ 24 → width 6 at 132 columns / 10 at 80, height 8 at 48 lines / 10 at 36, then tw.
  - A row is in range when: declared height ≤ h if one is declared, else used height ≤ round-up-to-6(h); and
    declared width ≤ w if one is declared, else used width ≤ w.
- `buildSoftFont`:
  - Dscs is 0..2 bytes in 0x20–0x2F followed by one final in 0x30–0x7E. A missing final is a
    `SoftFontError::Designator`.
  - Sixel data: `?`..`~` add a column, `/` moves down 6 rows, `;` ends the character. Every other byte,
    CR/LF included, is ignored.
  - Accumulate into a 16×32 scratch per character, ignoring columns ≥ 16 and rows ≥ 32. Track the used
    width and height, then `detectTargetCell`.
  - Clip text fonts to `textWidth` and centre them in `fullWidth` (offset = (fullWidth − textWidth) / 2),
    as in `_packAndCenterBitPatterns`.
  - Positions past 0x7F are dropped.
  - Pe=1 with an unchanged declaration, designator and charset size starts from `previous->glyphs`.
    Otherwise every glyph starts undefined. Pe=2 is handled by the store.
- Error glyph: Windows Terminal's 15×23 reverse-question-mark pattern (`_generateErrorGlyph`'s
  `inputBitPattern`), scaled nearest-neighbour to `fullWidth × fullHeight`.
- `SoftFontStore::define`:
  - Pe=2 drops every font before loading.
  - Each new `SoftFont` is a fresh `shared_ptr<SoftFont const>` (copy-on-write: existing pointers held by
    render buffers stay valid and unchanged).
  - `fontForDesignator` picks the most recently loaded match.
  - `generation()` increments on `define`, `clear` and `replaceAll`.

- [ ] **Step 1: Write the failing tests** in `SoftFont_test.cpp`, all `[SoftFont]`:
  - `TEST_CASE("decodeDecdld: Pcn is a position, not a code point")`:
    `decodeDecdld({.pfn=0,.pcn=1,...}).value().startPosition == 0x21`; with `.pcss=1,.pcn=0` → `0x20`.
  - `TEST_CASE("decodeDecdld: rejects what DEC rejects")`: a table with a `GENERATE` row for each of:
    `pfn=3` → `FontNumber`, `pcmw=1` → `CellMatrix`, `pss=5` → `FontSet`, `pt=3` → `Usage`, `pcss=2` →
    `CharsetSize`, `pcmw=2,pt=2` → `CellMatrix`, `pcmw=17` → `TooLarge`.
  - `TEST_CASE("detectTargetCell: one row per terminal")`: a `GENERATE` table covering every row above plus the
    three overrides. The real-font rows are mandatory:
    - vt100.net / cmatrix header `Pfn=0/1;Pcn=1;Pe=1;Pcmw=0;Pss=0;Pt=2`, used 15×12 → `{15,12,0}`.
    - larsbrinkhoff `P1;1;1{ @`, used 8×12 → `{10,10,8}`.
  - `TEST_CASE("buildSoftFont: a VT320 full-cell glyph is stored at 15x12 and at position 0x21")`, using a
    synthetic `B` font with two 15-column, two-sixel-row glyphs separated by `;` and containing CR/LF.
    Assert `glyphs[1]->width == 15`, `height == 12`, and exact `coverage` rows for glyph 1.
  - `TEST_CASE("buildSoftFont: columns past 16 do not spill into the next row")`.
  - `TEST_CASE("buildSoftFont: a VT220 text glyph is clipped to 8 and centred in 10")`: column 0 of the
    coverage is empty, and the input's column 0 appears in column 1.
  - `TEST_CASE("buildSoftFont: two-intermediate designator")`: `" @"` and `"%5"` round-trip into
    `designator`. A body with no final byte → `Designator`.
  - `TEST_CASE("buildSoftFont: Pe=1 keeps other glyphs, Pe=0 drops them")`.
  - `TEST_CASE("buildSoftFont: undefined positions are null and the error glyph is full-size")`.

  In `SoftFontStore_test.cpp` (`[SoftFontStore]`):
  - `"define then resolve"`: Defined. Undefined with the error glyph. `FontGone` for an unloaded font.
  - `"Pe=2 erases every font"`.
  - `"most recently loaded designator wins"`: font 1 `B`, then font 2 `B` → 2.
  - `"redefinition is copy-on-write"`: hold `resolve(ref).glyph`, redefine with different pixels, and check
    that the held pointer's `coverage` is unchanged while the new resolution has a different `id`.
  - `"identical glyphs share an id, different ones do not"` (the id is a content hash; it is the atlas key
    across every session one renderer draws).
  - `"an out-of-range ref resolves as FontGone"`: `{3, 0x21}`, `{0, 0x10}`, `{0, 0x80}`.
  - `"an invalid DECDLD leaves the store unchanged"`: `generation()` is unchanged.

- [ ] **Step 2: Run them and verify they fail to compile or fail.**
  Hash: `crispy::StrongHash` over width, height and coverage, folded to 64 bits; any collision-resistant
  64-bit hash already in the tree is fine.
  `cmake --build --preset clang-asan --target vtbackend_test && out/clang-asan/src/vtbackend/vtbackend_test "[SoftFont],[SoftFontStore]"`

- [ ] **Step 3: Implement** the interfaces above. Use `std::views::iota` and ranges, with no index loops.

- [ ] **Step 4: Run the tests and confirm they pass.** Same command as Step 2: all pass, with no warnings.

- [ ] **Step 5: Commit** `vtbackend: add a pure DECDLD soft font model with target-cell detection`

---

### Task 2: The per-cell soft-glyph reference in the grid

**Files:**
- Modify: `src/vtbackend/grid/LineSoA.hpp` (beside `imageFragments`, ~line 146), `LineSoA.cpp` (every
  `imageFragments` site: lines ~41, 111-116, 157, 204-212, 408), `CellProxy.hpp` (~118-130, 220, 302-360),
  `Line.cpp:221`, `SoAClusterWriter.hpp/.cpp`, and `src/vtbackend/screen/StatusLineExtensions.cpp` /
  `src/vtextension/Surface.cpp` wherever they reset or copy `imageFragments`.
- Test: `src/vtbackend/grid/LineSoA_test.cpp` (or the existing grid/line test file holding the
  `imageFragments` tests, so new cases sit beside their siblings).
- Create: `src/vtbackend/screen/Screen_softfont_test.cpp` (add to CMake), for the column-move cases.

**Interfaces:**
- Consumes: `SoftGlyphRef` (Task 1).
- Produces:
  - `LineSoA::SoftGlyphMap = std::unordered_map<uint16_t, SoftGlyphRef>`
  - `std::unique_ptr<SoftGlyphMap> LineSoA::softGlyphs`: 8 bytes on every line, not an `optional<map>`'s
    48–64, so lines without soft fonts pay nearly nothing (spec anti-goal). Deep-copy it wherever `LineSoA` is
    copied.
  - `CellProxy::softGlyph() const -> std::optional<SoftGlyphRef>`
  - `CellProxy::setSoftGlyph(SoftGlyphRef)`, which degrades the line to `LineShape::Spanning` exactly as
    `setImageFragment` does
  - `CellProxy::clearSoftGlyph()`

**Rule:** at every place `imageFragments` is reset, erased per column, cleared on a cell write, copied or
shifted, or tested in an emptiness or trivial-line check, `softGlyphs` gets the same treatment.

**Plus one site `imageFragments` lacks:** `moveColumns` (`LineSoA.cpp:234-237`, used by ICH/DCH/DECIC/DECDC at
`Screen.cpp:2161, 2202, 2225, 2449`) moves no side table at all. Add a shared helper that re-indexes a column
map for a move: drop destination entries not sourced from the move, shift `[src, src+count)` by `dst − src`.
Apply it to **both** `softGlyphs` and `imageFragments`. Fixing the latent image bug in the same helper is a call
made in this plan; say so in the commit.

**Hot path:** both side tables imply `LineShape::Spanning`, so put one `shape == Spanning` test in front of both
clears in `writeCellToSoA`, `writeAsciiToSoA`, `resetCell` and `setCharacter`. Ordinary lines then pay less than
they do today.
`CellProxy::empty()` stays codepoint-based, because a soft-glyph cell always carries a codepoint.

- [ ] **Step 1: Write the failing tests** (`[LineSoA][SoftGlyph]`):
  - `"setSoftGlyph degrades the line to Spanning"`.
  - `"writing a cell drops its soft glyph"`: set a ref at column 3, write `'x'` there through `CellProxy::write`,
    and check that `softGlyph()` is empty.
  - `"reset drops all soft glyphs"`.
  - `"copy/reflow carries soft glyphs with their cells"`: use the same helper the `imageFragments` reflow test
    uses. The ref at source column 5 lands at the shifted destination column. (Review Focus 1)
  - Screen-level, in `src/vtbackend/screen/Screen_softfont_test.cpp` (created here, extended in Task 3), set
    up with `CellProxy::setSoftGlyph` directly: `"ICH and DCH shift soft glyphs with their cells"` (ICH 2 at
    column 0 moves the ref from 3 to 5, DCH 2 moves it back, and no ref is left at the vacated column);
    `"DECIC and DECDC shift soft glyphs"`; and `"ICH shifts image fragments too"` for the shared fix.
    (Review Focus 3)
- [ ] **Step 2: Run** `vtbackend_test "[SoftGlyph]"` and confirm the tests fail.
- [ ] **Step 3: Implement** by mirroring each `imageFragments` site. Factor shared column-map helpers
  (`clearReplacedImageFragment`/`…Fragments` in `LineSoA.hpp:189-208`) into a template over the map type,
  rather than duplicating them.
- [ ] **Step 4: Run** `vtbackend_test` (the whole suite, since grid changes are broad) and confirm it is green.
- [ ] **Step 5: Measure.** `bench-headless` instruction counts on `ascii`, `utf8`, `tui` before and after this
  task: unchanged within ±0.1%, or better. Record the table in the commit message.
- [ ] **Step 6: Commit** `vtbackend: carry a soft-glyph reference per cell beside image fragments`

---

### Task 3: Wire DECDLD, designation and the write path to the new model

**Files:**
- Modify: `src/vtbackend/screen/Terminal.hpp` (remove `DRCSGlyph`, `DRCSCharset`, `defineDRCS`, `drcsCharset`,
  `clearDRCS`, `drcsDesignatorToFont`, `createDRCSImage`, `_drcsCharsets` and `_drcsDesignatorMap`; add
  `SoftFontStore _softFonts` with `softFonts() const`, `SoftFontStore& softFonts()`), `Terminal.cpp`
  (~8959-9060), `Screen.cpp` (`hookDECDLD` ~9137, `tryHandleSCS` ~3000-3090, `writeSoftFontGlyph` ~925, the
  RIS path that called `clearDRCS`), `Screen.hpp`.
- Modify: `src/vtbackend/screen/Screen_charset_test.cpp:713-838`, `BulkScan_test.cpp:368`,
  `state/InFlightTail_test.cpp:116-120` (fix their `Pcn=0`/`Pw=10` sequences to valid ones, so they keep
  testing what they meant to), and `examples/drcs-demo.cpp` (valid parameters).
- Test: `src/vtbackend/screen/Screen_softfont_test.cpp` (created in Task 2).

**Interfaces:**
- Consumes: `SoftFontStore` (Task 1), `CellProxy::setSoftGlyph` (Task 2).
- Produces: `Terminal::softFonts()` (const and non-const). Cells written through a soft set hold their source
  codepoint plus a `SoftGlyphRef`.

**Behaviour:**
- `hookDECDLD` collects the body and calls `_terminal->softFonts().define(params, body)`, then
  `_terminal->markScreenDirty()` so live redefinition reaches the next frame.
- `tryHandleSCS` consults `softFonts().fontForDesignator(designator)` **before** the standard-charset table.
  The two-byte fallback path stays.
- `writeSoftFontGlyph(font, sourceCodepoint)`:
  - If `softFonts().font(font)` is null, return `Absent` (the existing fall-through prints the mapped
    character).
  - Otherwise write `sourceCodepoint` as an ordinary width-1 cell in the current rendition, call
    `setSoftGlyph({font, position = sourceCodepoint & 0x7F})`, advance, mark dirty, consume the single
    shift, and return `Written`. This happens even if the position is undefined: the renderer shows the error
    glyph.
  - Keep the inline `activeDRCSFont()` gate from `582854fb4` unchanged.
- RIS calls `softFonts().clear()`. Soft reset leaves fonts alone (existing test at
  `Screen_charset_test.cpp:756`).
- The bulk text path seeds the grapheme run with `U' '` after a soft glyph (`Screen.cpp:826-828`), because
  the cell used to hold a space. Seed the stored codepoint instead, so the bulk path and
  `seedGraphemeStateFromLastCell` agree. Fix the stale comment at `BulkScan_test.cpp:368`.
- `Screen_charset_test.cpp:844-878` calls `drcsCharset(1)` and documents the ASCII fallback the spec replaces
  for a loaded font. Rewrite it to the new rule: an undefined position in a loaded font writes a reference.

- [ ] **Step 1: Write the failing tests** in `Screen_softfont_test.cpp` (`[screen][softfont]`), using `MockTerm`:
  - `"a VT320 font loaded at Pcn=1 renders at '!'"`: send a synthetic `ESC P0;1;1;0;0;2;0;0{B<two glyphs>ESC \`,
    then `ESC ( B`, then `!`. Assert that cell (0,0) has codepoint `'!'` and `softGlyph() == SoftGlyphRef{0, 0x21}`.
  - `"a raw 8-bit DECDLD (0x90 ... 0x9C) loads like the 7-bit form"`: the same font framed with `\x90` and
    `\x9c` with CR/LF inside the body, as the vt100.net `.fnt` files are. The resulting store is identical.
  - `"a loaded font takes over a standard designator"`: after loading `B`, `ESC ( B` selects the soft font.
    After RIS, `ESC ( B` is ASCII again.
  - `"copy yields the underlying characters"`: write `A!B` with `!` soft, select the line, and check that the
    selected text is `"A!B"`. Use the same selection-text helper other screen tests use. (Review Focus 4)
  - `"overwriting a soft cell with plain text drops the reference"`: write a soft `!`, then `ESC ( B` with the
    font erased (Pe=2 with no data), CUP to the cell, and write ASCII through the bulk path. `softGlyph()` is
    empty. (Review Focus 2)
  - `"erase drops the reference"`: ECH 1 over the soft cell.
  - `"an undefined position in a loaded font still writes a reference"`: `"` (0x22) with only `!` defined →
    `SoftGlyphRef{0,0x22}`.
  - `"a designated but erased font falls back to the plain character"`: Pe=2 with no glyphs, then `!` → no
    reference, codepoint `'!'`.
- [ ] **Step 2: Run** `vtbackend_test "[softfont]"` and confirm the tests fail.
- [ ] **Step 3: Implement** the behaviour above and delete the old DRCS members and `createDRCSImage`. Update
  the existing tests listed under Files. Update every remaining `drcsCharset`/`drcsDesignatorToFont` call site
  (`grep -rn "drcs" src/vtbackend`) except the state codec, which Task 6 owns. **Keep `KeyStringsState`
  fields 6 and 7** with their current types, always written empty (the old `DRCSCharset` shape moves into
  `TerminalState.hpp` as a codec-private placeholder struct). The v1 encoding then stays byte-identical, and
  `Golden_test.cpp` stays green until Task 6.
- [ ] **Step 4: Run** the full `vtbackend_test` and `vtconformance` (`ctest --preset=clang-asan -R "vtbackend|vtconformance"`)
  and confirm they are green. `vttest.11.2.vt320.cmd` must still pass.
- [ ] **Step 5: Commit** `vtbackend: route DECDLD through the soft font store and keep the real characters`

---

### Task 4: Resolve soft glyphs into the render buffer

**Files:**
- Modify: `src/vtbackend/render/RenderBuffer.hpp:73-93` (add the field), `src/vtbackend/screen/RenderBufferBuilder.cpp:430-452`,
  `RenderBufferBuilder.hpp:195`. `makeRenderCell` is `static`: pass it the `SoftFontStore const&` and the
  line's `softGlyphs` map (or null), so the per-line skip is one pointer test.
- Test: `src/vtbackend/screen/Screen_softfont_test.cpp` (render-buffer cases), plus a `[threading]` case.

**Interfaces:**
- Consumes: `CellProxy::softGlyph()`, `Terminal::softFonts().resolve()`.
- Produces: `std::shared_ptr<SoftGlyph const> RenderCell::softGlyph`. It is non-null exactly when the cell's
  font exists (the defined glyph, or the font's error glyph when undefined). It is null when the font is gone,
  in which case the cell renders its codepoint as normal text.

- [ ] **Step 1: Write the failing tests:**
  - `"render cell carries the defined glyph"`.
  - `"render cell carries the error glyph for an undefined position"`.
  - `"render cell has no soft glyph after RIS, and shows its codepoint"`.
  - `"redefinition shows on the next render buffer"`: the glyph `id` changes.
  - `TEST_CASE("soft font redefinition races rendering", "[softfont][threading]")`: one thread loops DECDLD
    redefinitions through `MockTerm`, and the other loops `refreshRenderBuffer` and reads every
    `softGlyph->coverage`. Run 1000 iterations each. It must be clean under TSan
    (`ctest --preset=clang-tsan -R vtbackend`).
- [ ] **Step 2: Run** and confirm the tests fail.
- [ ] **Step 3: Implement** in `makeRenderCell`. The builder runs under the state lock, so `resolve()` there is
  safe. Skip the lookup entirely when the line has no `softGlyphs` (one `optional` test per line).
- [ ] **Step 4: Run** the `[softfont]` tests and the TSan `[threading]` test and confirm both pass.
- [ ] **Step 5: Commit** `vtbackend: resolve soft glyphs into render cells under the state lock`

---

### Task 5: `SoftFontRenderer` — crisp alpha tiles coloured at draw time

**Files:**
- Create: `src/vtrasterizer/SoftFontRenderer.hpp`, `SoftFontRenderer.cpp`, `SoftFontRenderer_test.cpp`
- Modify: `src/vtrasterizer/TextRenderer.hpp/.cpp` (own a `SoftFontRenderer` beside `_boxDrawingRenderer`, and
  wire `setRenderTarget`/`setTextureAtlas`/`clearCache`/`inspect` the same way, ~282-330), `TextRenderer::renderCell`
  (~473), and `CMakeLists.txt`. `TextClusterGrouper` needs no change: `forceGroupStart()`/`forceGroupEnd()`
  exist (`TextClusterGrouper.hpp:47-48`).

**Interfaces:**
- Consumes: `RenderCell::softGlyph` (Task 4).
- Produces:

```cpp
/// Scales a soft glyph to @p target with nearest-neighbour sampling; coverage 1 -> 0xFF.
[[nodiscard]] std::vector<uint8_t> scaleSoftGlyph(vtbackend::SoftGlyph const&, vtbackend::ImageSize target);

enum class SoftGlyphDraw : uint8_t { AtlasFull, Drawn };

class SoftFontRenderer: public Renderable
{
  public:
    explicit SoftFontRenderer(GridMetrics const&);
    /// @return Drawn, or AtlasFull, in which case the caller falls back to drawing the cell's text.
    [[nodiscard]] SoftGlyphDraw render(vtbackend::CellLocation, vtbackend::SoftGlyph const&, vtbackend::RGBColor, vtbackend::LineFlags);
};
```

**Behaviour:**
- `dst(x, y) = src(x * w / W, y * h / H)`, where w×h is the glyph size and W×H is the target.
- The target is the cell size. With `DoubleWidth`, the width is doubled and two tiles are emitted
  (`subIndex` 0/1). With `DoubleHeightTop`/`Bottom`, the height is doubled and the matching half is taken.
  Mirror `BoxDrawingRenderer::render`/`createTileData`, including its Y-flip (`invertY`).
- Tiles use `atlas::Format::Red` with `FRAGMENT_SELECTOR_GLYPH_ALPHA`, and the colour comes from
  `RenderTile::color` at draw time.
- Atlas key: `crispy::StrongHash { 0x534F4654 /* 'SOFT' */, uint32(id >> 32), uint32(id), (flags & DW|DHT|DHB) << 1 | subIndex }`.
  The tag word keeps it disjoint from box-drawing keys. No colour goes in the key.
- `TextRenderer::renderCell`: if `cell.softGlyph`, call `forceGroupEnd()`, then `_softFontRenderer.render(position,
  *cell.softGlyph, cell.attributes.foregroundColor, cell.attributes.lineFlags)`, then `forceGroupStart()` for
  the next cell. On `AtlasFull`, fall through to the normal text path, as `BoxDrawingRenderer::render`'s
  failure does (`BoxDrawingRenderer.cpp:2107-2119`). `foregroundColor` already reflects reverse video, selection and so on, because
  `RenderBufferBuilder` resolves it, which is the whole point of goal 3.

- [ ] **Step 1: Write the failing tests** (`[SoftFontRenderer]`, using `MockAtlasBackend` and `bitmapRows` from
  `RendererTestHelpers.hpp`):
  - `"scaleSoftGlyph is nearest-neighbour and fills the target"`: a 2×2 checkerboard scaled to 4×6 gives exact
    rows, and every value is 0 or 0xFF (no intermediate values).
  - `"a 15x12 glyph fills a 9x18 cell exactly"`: the dimensions match, and the first and last rows and columns
    sample the source edges.
  - `"one upload for many cells"`: rendering the same glyph in 100 cells with 3 colours gives 1 upload command
    and 100 render commands, with per-command colours matching.
  - `"double-width emits two tiles from a doubled glyph"`; `"double-height top and bottom take the right halves"`.
  - `"cell size change regenerates the tile"`: reconfigure the atlas with a new cell size and render again. The
    upload bitmap has the new size. (Review Focus 5)
- [ ] **Step 2: Run** `vtrasterizer_test "[SoftFontRenderer]"` and confirm the tests fail.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the full `vtrasterizer_test` and confirm it is green.
- [ ] **Step 5: Commit** `vtrasterizer: render soft font glyphs as crisp alpha tiles coloured at draw time`

---

### Task 6: Persist soft fonts and references across a live upgrade

**Files:**
- Modify: `src/vtbackend/state/TerminalState.hpp:349-369` and `TerminalState.cpp:368-369, 691-694`,
  `src/vtbackend/state/GridState.cpp` (~463-480 encode, ~541 decode: add a `LineField::SoftGlyphs` beside
  `LineField::ImageFragments`).
- Test: `src/vtbackend/state/ScreenState_test.cpp` (or the codec round-trip test that covers image fragments),
  `InFlightTail_test.cpp`.

**Rules (calls made in this plan):**
- **This task bumps `TerminalStateFormatVersion` to 2.** `FieldEncoder` writes every field unconditionally
  (`StateFields.hpp:273-276`), and `Golden_test.cpp:82-90` pins v1 byte for byte, so the remedy it prescribes is
  a new version with a new golden, never an edit of the old one. Record a v2 golden beside the v1 one, and keep
  decoding v1.
- Fields 6 and 7 (`drcsCharsets`, `drcsDesignators`) are **retired** in v2: they are no longer written, and a
  v1 decode ignores their contents. Fonts written by the old code were stored under wrong positions and never
  displayed, so dropping them loses nothing visible. Remove the Task 3 placeholder struct.
- The fonts travel in a new field 8, holding per font: number, designator, the raw `DecdldParameters`-derived
  declaration, geometry, and per defined position the width, height and coverage. Encode coverage packed at
  1 bit per pixel. Ids are content hashes, so nothing about identity is persisted: the decoder rebuilds glyphs
  with `makeSoftGlyph`/`makeSoftFont`, then calls `SoftFontStore::replaceAll`.
- Line field `SoftGlyphs` (`LineField` 30) holds (column, font, position) triples, sorted by column, the same
  way `ImageFragments` is encoded. The decoder rejects a font > 2 or a position outside 0x20..0x7F, as
  `getFragments` (`GridState.cpp:541-562`) rejects bad columns.

- [ ] **Step 1: Write the failing tests:**
  - `"soft fonts and per-cell references round-trip"`: load a font, write soft cells into the page and
    scrollback, capture, restore into a fresh terminal, and compare `fonts()` coverage and every cell's
    `softGlyph()`.
  - `"a v1 state with DECDLD fields still decodes"` (the v1 golden) and `"the v2 golden is byte for byte"`.
  - `"an out-of-range soft-glyph triple is rejected"`.
  - Extend the existing `InFlightTail` DECDLD case to use the corrected sequence (Task 3) and to still be exact
    at every byte offset.
- [ ] **Step 2: Run** and confirm the tests fail.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `ctest --preset=clang-asan -R vtbackend` and confirm it is green.
- [ ] **Step 5: Commit** `vtbackend: carry soft fonts and soft-glyph cells through the state codec`

---

### Task 7: Mirror soft fonts to daemon clients

**Files:**
- Modify:
  - `src/vthost/proto/Negotiation.hpp`: add `SoftFonts = 21` after `CellPixelSize = 20`, with its row in the
    capability table.
  - `src/vthost/proto/Pdu.hpp/.cpp`: add `PduType::SoftFontState = 45`, new structs, encode/decode table rows,
    and an appended `Delta::softGlyphCells` field.
  - `src/vthost/NativeSession.cpp`: emit the PDU and the cells beside `appendImageCells` (~213).
  - `src/vthost/GridWire.*`.
  - `src/vthost/client/ScreenMirror.hpp/.cpp`: apply both, as `applyImages` does (~911).
  - The `RemoteScreen` holder of `imageCells`.
  - `src/vthost/testing/GridParity.cpp`.
- Test: `src/vthost/proto/Pdu_test.cpp` (round-trip), a new `src/vthost/DaemonSoftFont_test.cpp` modelled on
  `DaemonImage_test.cpp`, and `client/ScreenMirror_test.cpp`.

**Interfaces:**

```cpp
struct WireSoftGlyph { uint8_t position; uint8_t width; uint8_t height; std::vector<std::byte> bits; }; ///< 1 bpp, rows byte-aligned
struct WireSoftFont
{
    uint8_t fontNumber; std::string designator; uint8_t charsetSize; uint8_t usage;
    uint8_t fullWidth; uint8_t fullHeight; uint8_t textWidth;
    std::vector<WireSoftGlyph> glyphs; WireSoftGlyph errorGlyph;
};
struct SoftFontState { uint64_t session = 0; uint64_t generation = 0; std::vector<WireSoftFont> fonts; };
struct SoftGlyphCellEntry { int64_t stableId = 0; uint16_t column = 0; uint8_t font = 0; uint8_t position = 0; };
// Delta: std::vector<SoftGlyphCellEntry> softGlyphCells;   // appended at the END of Delta's fields
```

**Behaviour:**
- **Gating:** only when both ends negotiated `Capability::SoftFonts`. Without it, the daemon sends neither,
  and the client shows the cells' real codepoints, which are already in the wire lines (spec call).
- **Sending fonts:** the daemon sends `SoftFontState` on attach, and whenever `softFonts().generation()`
  differs from the last one sent to that connection. It is always queued **before** the `Delta` that might
  reference it. An empty `fonts` vector means none.
- **Applying fonts on the client:** the mirror rebuilds `SoftFont`s with `makeSoftGlyph`/`makeSoftFont`, and
  calls `terminal.softFonts().replaceAll(...)` **under the terminal lock**, as `applyImages` does
  (`ScreenMirror.cpp:988`), then marks the screen dirty.
- **Validation:** the decoder rejects a `SoftGlyphCellEntry` with font > 2 or a position outside 0x20..0x7F,
  and a `WireSoftGlyph` whose `bits` size does not match its width and height.
- **Chunked snapshots:** regroup `softGlyphCells` per row into each snapshot piece exactly as `imageCells` are
  (`NativeSession.cpp:1569-1589`). The mirror clears a row's side-table entries when it takes the row, so a ref
  must travel in the same piece as its row.
- **Cell references:** `softGlyphCells` are applied after the wire lines, with `setSoftGlyph`. `GridParity`
  compares `softGlyph()` per cell.

- [ ] **Step 1: Write the failing tests:**
  - PDU round-trip for `SoftFontState`. A `Delta` with and without `softGlyphCells` decodes on an old-layout
    reader (the appended-field rule).
  - `DaemonSoftFont_test`:
    - `"a font defined before attach reaches the client"`.
    - `"redefinition after attach updates the client's glyph"`.
    - `"a client without the capability sees the underlying characters"`.
    - `"RIS on the server makes the client fall back to text"`.
    - `"soft-glyph cells in a chunked snapshot survive"`: a scrollback larger than `_snapshotPieceBytes`, with
      soft cells on rows in the second and later pieces.
    - `"an out-of-range soft-glyph entry is rejected"`.
  - `ScreenMirror_test`: `"soft glyph cells mirror with parity"`.
- [ ] **Step 2: Run** `ctest --preset=clang-asan -L daemon` and confirm the new tests fail.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `ctest --preset=clang-asan -L daemon` and confirm it is green.
- [ ] **Step 5: Commit** `vthost: mirror soft fonts and soft-glyph cells to attached clients`

---

### Task 8: Verify on real fonts, measure, document

**Files:**
- Modify: `metainfo.xml` (release note, via `/add-release-note`), `docs/` VT sequence reference if DECDLD is
  listed there (`grep -rn DECDLD docs`), and `examples/drcs-demo.cpp`.

- [ ] **Step 1: Visual check, offscreen** (`QT_QPA_PLATFORM=offscreen`, captured with `grabWindow()` to PNG,
  never OS screenshots).
  - Download into a fresh scratch directory: `https://vt100.net/dec/vt320/{boothill,future,gothic,jetpac,mercy,powerslave}.fnt`,
    `https://github.com/larsbrinkhoff/sixel` (`vt220.txt`, `birger.txt`), and cmatrix's `vt320-font.h` from
    `https://github.com/jhamby/cmatrix`.
  - For each, render the font file followed by `ESC ( B` (or `ESC ( @`/`ESC ( SP @`) and all 94 positions,
    once at the default font size and once after a font-size change, plus one DECDWL line.
  - Compare by eye against the vt100.net previews. Then change SGR (reverse video, a colour) and select a range,
    and confirm the glyphs recolour.
  - Do this with a *display test once per phase*. This is the only window-opening run.
- [ ] **Step 2: Performance.** Run `bench-headless` and Callgrind before (on `origin/master`) and after.
  `ascii`, `utf8` and `tui` instruction counts must be unchanged (±0.1%). Record the table for the PR.
- [ ] **Step 3: Docs.** Add a release note entry ("Soft fonts (DECDLD) now render real-world fonts…, #467")
  and fix `drcs-demo` if it still uses invalid parameters.
- [ ] **Step 4: Full suite.** `ctest --preset=clang-asan` and `ctest --preset=clang-tsan -R "vtbackend|vthost"`
  must both be green, with zero warnings.
- [ ] **Step 5: Run `/simplify`, then `/code-review`.** Address every finding.
- [ ] **Step 6: Commit** `docs: release note and demo for finished soft fonts (#467)`
