# Per-screen Link Detection and an Even Dotted Underline — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make automatic URL/file-path detection switchable per screen group (primary vs. every other page), with paths off on non-primary screens by default, and redraw the dotted underline as evenly spaced round dots continuous across cells.

**Architecture:** A small `LinkDetectionPolicy` value type in `vtbackend` decides per page which link categories are detected; `LinkDetector` (background collect, `scanNow`) and `Terminal::lookUpLinkAt` drop disabled categories *before* classification, so nothing is probed. The policy reaches `Settings` from a new profile entry `link_detection`, applied live like `setLinkPatterns`. The dotted underline gets a pure geometry function in `UnderlineGeometry.hpp`; `DecorationRenderer` uploads one tile per phase and picks the phase from the column.

**Tech Stack:** C++23, Catch2, Qt6 (settings page only), yaml-cpp config.

**Spec:** `docs/superpowers/specs/2026-10-22-link-detection-per-screen-and-dotted-underline-design.md`

## Global Constraints

- Defaults: primary screen URLs **on**, file paths **on**; other screens URLs **on**, file paths **off**.
- "Other screens" = every page except `PageIndex(0)`. Decide by `page == PageIndex(0)`, never by "is the alternate screen".
- URLs switch covers `LinkKind::Url` and `LinkKind::Custom`; file-paths switch covers `LinkKind::Path` and any target that `isFileUrl()`.
- OSC 8 hyperlinks and hint mode are never affected.
- YAML keys: `link_detection.primary_screen.{urls,file_paths}`, `link_detection.other_screens.{urls,file_paths}`.
- Dot diameter `d = max(2, round(1.5 × max(1, thickness)))`, clamped to `max(1, underlinePosition)` and to `MaxDottedUnderlinePitch / 2`; pitch `2d`; `MaxDottedUnderlinePitch = 32`.
- No `bool` parameters in `vtbackend` APIs for the policy (AGENT.md "enum class over bool"); `bool` is allowed only in the `Config` structs (YAML boundary).
- `-Werror`, clang-format, no NOLINT. Configure with `PACE_SCHEDULER=127.0.0.1:6674` (already baked into `out/clang-asan`).
- Every commit ends with `Signed-off-by: Christian Parpart <christian@parpart.family>`.

## Review Focus

1. Changing the policy by live reload while the alternate screen is shown → the detector is replaced, the hovered link and tooltip are forgotten, and paths disappear on the next cycle. Test in Task 2 (`aNewPolicyReplacesTheDetector`).
2. A printed `file://` URL on the alternate screen under the defaults → no link, because it follows the file-paths switch. Test in Task 2 (`aFileUrlFollowsTheFilePathSwitch`).
3. Every switch off → no detector at all, `lookUpLinkAt` finds no URL, and an OSC 8 link still answers. Test in Task 2 (`withEverythingOffOnlyOsc8LinksRemain`).
4. Cells narrower than the pitch (7 px wide, thickness 3 → pitch 10) → dots still evenly spaced across cells. Covered by Task 5's width × thickness sweep.
5. Double-width lines (DECDWL) → phase is taken from the scaled column, so dots stay evenly spaced. Test in Task 6 (`dottedUnderlineContinuesOnADoubleWidthLine`).

---

### Task 1: The policy type and link categories

**Files:**
- Create: `src/vtbackend/links/LinkDetectionPolicy.hpp`
- Modify: `src/vtbackend/screen/PrintedLinks.hpp` / `.cpp` (add `linkCategoryOf`)
- Modify: `src/vtbackend/CMakeLists.txt` (header list, test source)
- Test: `src/vtbackend/links/LinkDetectionPolicy_test.cpp` (new, added to `vtbackend_test`)

**Interfaces:**
- Produces:
  ```cpp
  namespace vtbackend {
  enum class LinkCategory : uint8_t { Url = 0, FilePath };          // what a switch names
  enum class DetectLinks : uint8_t { No = 0, Yes };
  struct ScreenLinkDetection {
      DetectLinks urls = DetectLinks::Yes;
      DetectLinks filePaths = DetectLinks::Yes;
      [[nodiscard]] constexpr bool detects(LinkCategory category) const noexcept;
      [[nodiscard]] constexpr bool detectsAny() const noexcept;
      bool operator==(ScreenLinkDetection const&) const = default;
  };
  struct LinkDetectionPolicy {
      ScreenLinkDetection primaryScreen { .urls = DetectLinks::Yes, .filePaths = DetectLinks::Yes };
      ScreenLinkDetection otherScreens { .urls = DetectLinks::Yes, .filePaths = DetectLinks::No };
      [[nodiscard]] constexpr ScreenLinkDetection forPage(PageIndex page) const noexcept;
      [[nodiscard]] constexpr bool detectsAnywhere() const noexcept;
      bool operator==(LinkDetectionPolicy const&) const = default;
  };
  // PrintedLinks.hpp, beside isFileUrl():
  [[nodiscard]] LinkCategory linkCategoryOf(LinkKind kind, std::string_view target) noexcept;
  }
  ```

- [ ] **Step 1: Write the failing tests** in `LinkDetectionPolicy_test.cpp`:
  ```cpp
  TEST_CASE("LinkDetectionPolicy.defaultsDetectPathsOnThePrimaryScreenOnly", "[links]")
  {
      constexpr auto policy = LinkDetectionPolicy {};
      STATIC_CHECK(policy.forPage(PageIndex(0)).detects(LinkCategory::Url));
      STATIC_CHECK(policy.forPage(PageIndex(0)).detects(LinkCategory::FilePath));
      for (auto const page: { 1, 2, 15 }) {
          CHECK(policy.forPage(PageIndex(page)).detects(LinkCategory::Url));
          CHECK_FALSE(policy.forPage(PageIndex(page)).detects(LinkCategory::FilePath));
      }
      CHECK(policy.detectsAnywhere());
  }
  TEST_CASE("LinkDetectionPolicy.everythingOffDetectsNowhere", "[links]") // all four No → !detectsAnywhere(), !detectsAny() for pages 0 and 1
  TEST_CASE("LinkDetectionPolicy.categories", "[links]")
  {
      CHECK(linkCategoryOf(LinkKind::Url, "https://x.y/") == LinkCategory::Url);
      CHECK(linkCategoryOf(LinkKind::Custom, "JIRA-1") == LinkCategory::Url);
      CHECK(linkCategoryOf(LinkKind::Path, "src/a.cpp") == LinkCategory::FilePath);
      CHECK(linkCategoryOf(LinkKind::Url, "file:///etc/hosts") == LinkCategory::FilePath);
      CHECK(linkCategoryOf(LinkKind::Url, "FILE://host/x") == LinkCategory::FilePath);
  }
  ```
- [ ] **Step 2: Run and see it fail to compile:** `cmake --build --preset clang-asan --target vtbackend_test` → missing header.
- [ ] **Step 3: Implement** the header (Doxygen on every member) and `linkCategoryOf` (a `switch` over `LinkKind`; `Url` checks `isFileUrl(target)`).
- [ ] **Step 4: Run:** `out/clang-asan/src/vtbackend/vtbackend_test "LinkDetectionPolicy.*"` → all pass.
- [ ] **Step 5: Commit** `vtbackend: add LinkDetectionPolicy, per-screen switches for automatic link detection`.

### Task 2: Detector, vi motions and lookups follow the policy

**Files:**
- Modify: `src/vtbackend/screen/Settings.hpp` (field `LinkDetectionPolicy linkDetection {};` beside `linkPatterns`)
- Modify: `src/vtbackend/screen/LinkDetector.hpp` / `.cpp`
- Modify: `src/vtbackend/screen/Terminal.hpp` / `.cpp` (`makeLinkDetector`, `lookUpLinkAt`, new setter)
- Test: `src/vtbackend/screen/Terminal_links_test.cpp`, `src/vtbackend/input/vi/ViCommands_test.cpp`

**Interfaces:**
- Consumes: Task 1.
- Produces:
  - `LinkDetector(LinkDetectionServices services, std::shared_ptr<LinkPatterns const> patterns, LinkDetectionPolicy policy, std::function<void()> requestRedraw);` The policy is stored in `State` and is fixed for the detector's lifetime. Add `[[nodiscard]] LinkDetectionPolicy const& policy() const noexcept;`.
  - `ScanBatch` gains `ScreenLinkDetection screen;`. `matchLine(State const&, LineJob const&, LinkMachine const&, ScreenLinkDetection screen)` skips a candidate when `!screen.detects(linkCategoryOf(candidate.kind, candidate.target))`, before `classifyPrintedLink` is called.
  - `collect()` returns without scheduling a match when `!policy.forPage(source.linkPage()).detectsAny()`. It still clears `_firstPending`/`_lastChange`, so `nextDue()` reports nothing pending. `scanNow()` returns `{ adoptFinished(source), 0 }` in that case, without reading any line.
  - `void Terminal::setLinkDetectionPolicy(LinkDetectionPolicy policy);` works like `setLinkPatterns`: it does nothing when the policy is equal, and otherwise replaces the detector, resets `_linkScanDue` and clears `_hoveringLinkId`. `makeLinkDetector()` also returns `nullptr` when `!_settings.linkDetection.detectsAnywhere()`.
  - In `Terminal::lookUpLinkAt` (the on-demand fallback), the `find_if` also requires `_settings.linkDetection.forPage(_displayedPage).detects(linkCategoryOf(found.kind, found.target))`.

- [ ] **Step 1: Write the failing tests** in `Terminal_links_test.cpp`, using `DetectingTerm`, `term.files.addFile`, `setCurrentWorkingDirectory` and `inWork()` as `eachRowsPathsResolveWhereItsCommandRan` does:
  - `Terminal.links.pathsAreNotDetectedOnTheAlternateScreenByDefault`: `notes.txt https://x.y/` printed on the primary screen → both targets detected. After `\033[?1049h\033[H` and printing the same text → `targetAt(0,0).empty()`, the URL is detected, and `term.probe.pending() == 0` (nothing was queued for probing).
  - `Terminal.links.otherScreensIncludeEveryNonPrimaryPage`: the same check on page 3 via `\033[3 P` (pattern as in `everyPageHasLinksOfItsOwn`).
  - `Terminal.links.eachSwitchGovernsItsScreenAndCategory`: use `GENERATE` over all 16 policies and check the four (screen, category) outcomes against the policy.
  - `Terminal.links.aFileUrlFollowsTheFilePathSwitch` (Review Focus 2).
  - `Terminal.links.aNewPolicyReplacesTheDetector` (Review Focus 1): while on the alternate screen, call `setLinkDetectionPolicy` with `otherScreens.filePaths = Yes` → `linkDetector()` is a different pointer, and after `cycle()` the path is detected. Setting an equal policy keeps the same pointer.
  - `Terminal.links.withEverythingOffOnlyOsc8LinksRemain` (Review Focus 3): `linkDetector() == nullptr`, `lookUpLinkAt` on a printed URL has no `target`, and an OSC 8 link written with `\033]8;;https://h/\033\\h\033]8;;\033\\` still returns `lookup.hyperlink`.
  - `Terminal.links.aLookupOnTheAlternateScreenFindsNoPathByDefault`: on an unscanned row, `lookUpLinkAt` on a path → `!printedPath && !pathToProbe`.
  - `Terminal.links.hintModeStillOffersPathsOnTheAlternateScreen`: copy how `hintModeAsksTheCacheDetectionFilled` starts hint mode, then assert that the path appears among the hint matches.

  In `ViCommands_test.cpp`, `vi.link: ]x skips paths on the alternate screen unless enabled`: on the alternate screen print `a notes.txt b https://x.y/`, with `notes.txt` present in `files`. `]x` goes to column 14 (the URL). After setting `otherScreens.filePaths = Yes`, `gg0]x` goes to column 2.
- [ ] **Step 2: Run** `out/clang-asan/src/vtbackend/vtbackend_test "[links]"` → the new cases fail (paths are still detected, and the setter is missing).
- [ ] **Step 3: Implement** the interfaces above. Update every `LinkDetector` construction site (`Terminal::makeLinkDetector`, plus any in tests: `grep -rn "LinkDetector(" src`).
- [ ] **Step 4: Run** `out/clang-asan/src/vtbackend/vtbackend_test "[links]"` and `"[vi]"` → all pass, including the existing `everyPageHasLinksOfItsOwn` and `theAlternateScreenHasLinksOfItsOwn`. Those use URLs only and must stay green unchanged.
- [ ] **Step 5: Commit** `vtbackend: detect links per screen by the profile's policy`.

### Task 3: The `link_detection` profile entry

**Files:**
- Modify: `src/contour/config/Config.hpp` (structs, `ConfigEntry`, `loadFromEntry` decl, `format` overload, `linkDetectionPolicyOf` decl)
- Modify: `src/contour/config/Config.cpp` (load at the profile loader beside `hyperlink_decoration` ~line 1636, `sessionSettings` ~line 873, conversion)
- Modify: `src/contour/config/ConfigDocumentation.hpp` (`LinkDetectionConfig`, `LinkDetectionWeb`, `using LinkDetection = DocumentationEntry<…>`)
- Modify: `src/contour/session/TerminalSession.cpp` ~5526 (apply live)
- Modify: `docs/configuration/links.md`
- Test: `src/contour/config/Config_test.cpp`

**Interfaces:**
- Consumes: Task 2 (`Settings::linkDetection`, `Terminal::setLinkDetectionPolicy`).
- Produces:
  ```cpp
  namespace contour::config {
  /// One screen group's switches, as YAML spells them (serialisation boundary: bool by design).
  struct ScreenLinkDetectionConfig { bool urls = true; bool filePaths = true; bool operator==(…) const = default; };
  struct LinkDetectionConfig {
      ScreenLinkDetectionConfig primaryScreen { .urls = true, .filePaths = true };
      ScreenLinkDetectionConfig otherScreens { .urls = true, .filePaths = false };
      bool operator==(LinkDetectionConfig const&) const = default;
  };
  // TerminalProfile:
  ConfigEntry<LinkDetectionConfig, documentation::LinkDetection> linkDetection {};
  [[nodiscard]] vtbackend::LinkDetectionPolicy linkDetectionPolicyOf(LinkDetectionConfig const& config) noexcept;
  }
  ```
  Documentation template (the writer's reflection walk emits it through `format(doc, v)` with four values in this order):
  ```
  {comment} Which links are found automatically in printed text, per screen. OSC 8 hyperlinks are
  {comment} always links, and hint mode always offers everything.
  {comment} primary_screen: the normal shell screen. other_screens: the alternate screen (vim, less,
  {comment} htop, ...) and every further page.
  link_detection:
      primary_screen:
          urls: {}
          file_paths: {}
      other_screens:
          urls: {}
          file_paths: {}
  ```

- [ ] **Step 1: Write the failing tests** (`[config][links]`), following `hyperlink_decoration.mode reads, writes and defaults`:
  - `Config: link_detection defaults to paths on the primary screen only`: with no key present, the profile's value equals `LinkDetectionConfig {}`, and `linkDetectionPolicyOf` of it equals `vtbackend::LinkDetectionPolicy {}`.
  - `Config: link_detection reads every switch`: YAML sets all four to the opposite of their defaults → each field is read back flipped.
  - `Config: a partial link_detection keeps the other defaults`: only `other_screens: { file_paths: true }` → the other three keep their defaults.
  - `Config: link_detection round-trips through the writer`: `defaultConfigString()` contains `link_detection:`, `other_screens:` and `file_paths: false`. Loading the written text back gives the same value.
  - `Config: sessionSettings carries the link detection policy`: `sessionSettings(config, *profile, …).linkDetection == linkDetectionPolicyOf(profile->linkDetection.value())`.
- [ ] **Step 2: Run** `out/clang-asan/src/contour/config/contour_config_test "[links]"` (check the target name with `ctest -N -R config`) → fails to compile.
- [ ] **Step 3: Implement.** Missing keys keep their defaults. A non-bool value is reported the same way other bool keys report it. In `TerminalSession`, call `_terminal.setLinkDetectionPolicy(configured.linkDetection);` right after `setLinkPatterns`. The tooltip reset there already covers the replaced detector. Document the section in `docs/configuration/links.md` with the defaults table from the spec.
- [ ] **Step 4: Run** the config tests → pass. Then run `ctest --preset=clang-asan -R "config|session"` → pass.
- [ ] **Step 5: Commit** `config: add link_detection, per-screen switches for URLs and file paths`.

### Task 4: Settings page switches

**Files:**
- Modify: `src/contour/window/SettingsController.cpp` (the `"Links"` group, after `hyperlink_decoration_hover`)
- Test: `src/contour/window/SettingsController_test.cpp`

**Interfaces:**
- Consumes: Task 3 (`TerminalProfile::linkDetection`).
- Produces: four `boolField` rows in group `"Links"`:

  | key | label |
  |---|---|
  | `link_detection_primary_urls` | Detect URLs on the primary screen |
  | `link_detection_primary_file_paths` | Detect file paths on the primary screen |
  | `link_detection_other_urls` | Detect URLs on other screens |
  | `link_detection_other_file_paths` | Detect file paths on other screens |

  Help text for the two "other" rows: "Other screens are the alternate screen used by full-screen programs (vim, less, htop) and any further page." Help text for all four ends with: "OSC 8 hyperlinks are always links." The accessor returns `p.linkDetection.value().otherScreens.filePaths` (and so on) by reference, like `background_blur`.

- [ ] **Step 1: Write the failing test** `SettingsController: link detection switches edit link_detection` (`[settings][links]`). After `newProfile("main")`, every key above exists, its `group` is `"Links"` and its `type` is `"bool"`. The values are `true, true, true, false`. `setProfileField("link_detection_other_file_paths", true)` flips the row's value, and the draft profile's `linkDetection.value().otherScreens.filePaths` becomes `true`. Also check that `profileFields()` filtered by the text "file paths" still finds both path rows, so search works. Use the same filter mechanism the existing search test uses (`grep -n "filter" SettingsController_test.cpp`).
- [ ] **Step 2: Run** `out/clang-asan/src/contour/contour_gui_test "[settings][links]"` (confirm the binary with `ctest -N -R settings`) → fails.
- [ ] **Step 3: Implement** the four rows.
- [ ] **Step 4: Run** the same command → pass.
- [ ] **Step 5: Commit** `settings: switches for per-screen link detection`.

### Task 5: Dotted underline geometry

**Files:**
- Modify: `src/vtrasterizer/UnderlineGeometry.hpp`
- Test: `src/vtrasterizer/UnderlineGeometry_test.cpp`

**Interfaces:**
- Produces:
  ```cpp
  namespace vtrasterizer {
  /// Upper bound of the dot pitch, and so of the phase tiles the renderer reserves.
  inline constexpr int MaxDottedUnderlinePitch = 32;
  struct DottedUnderlineGeometry {
      int diameter = 2;    ///< Dot diameter in pixels.
      int pitch = 4;       ///< Distance between dot starts: 2 * diameter.
      int phaseCount = 1;  ///< Distinct tiles a line needs: pitch / gcd(cellWidth, pitch).
      int tileHeight = 1;  ///< max(1, underlinePosition); the dot's top row is the tile's top row.
  };
  [[nodiscard]] constexpr DottedUnderlineGeometry dottedUnderlineGeometry(int cellWidth, int underlinePosition, int underlineThickness) noexcept;
  /// Which phase tile column @p column draws: column % phaseCount.
  [[nodiscard]] constexpr int dottedUnderlinePhaseOf(DottedUnderlineGeometry const& geometry, int column) noexcept;
  /// Antialiased coverage 0..255 of pixel (x, y), top-origin, in the tile of @p phase.
  /// The pixel's absolute x is (phase * cellWidth) % pitch + x. A disc of the given diameter starts at every multiple of pitch,
  /// with its centre at (k * pitch + d/2, d/2). Coverage comes from 4x4 supersampling.
  [[nodiscard]] constexpr uint8_t dottedUnderlineCoverage(DottedUnderlineGeometry const& geometry, int cellWidth, int phase, int x, int y) noexcept;
  }
  ```

- [ ] **Step 1: Write the failing tests** (`[underlinegeometry]`):
  - `UnderlineGeometry.dottedSizes`: for position 9 and width 10, thickness 1 → `{d=2, pitch=4}`, thickness 2 → `{3, 6}`, thickness 3 → `{5, 10}`. Width 9 with thickness 1 → `phaseCount == 4`, width 10 with thickness 1 → `phaseCount == 2`. Position 1 with thickness 3 → `diameter == 1`, `tileHeight == 1`.
  - `UnderlineGeometry.dottedDotsAreEvenlySpacedAcrossCells`: generate `width` from {7, 8, 9, 10, 11, 17, 18, 20} and `thickness` from {1, 2, 3}, with position 9. Concatenate the centre row (`y = d/2`) of `2 * phaseCount` consecutive cells, each using phase `dottedUnderlinePhaseOf(g, column)`. Collect the x positions where coverage rises from 0 to >0. Every difference between consecutive positions must equal `g.pitch`, and there must be at least 2 of them.
  - `UnderlineGeometry.dottedDotsStayInsideTheTile`: for every pixel with `y >= tileHeight` or `x` outside `[0, width)`, coverage is never asked for. Every dot's full diameter fits in rows `[0, tileHeight)`, so `diameter <= tileHeight`.
- [ ] **Step 2: Run** `out/clang-asan/src/vtrasterizer/vtrasterizer_test "[underlinegeometry]"` → fails to compile.
- [ ] **Step 3: Implement.** Use `std::lround` for 1.5 × thickness, `std::gcd` for the phase count, and clamp the diameter as the Global Constraints say.
- [ ] **Step 4: Run** the same command → pass.
- [ ] **Step 5: Commit** `vtrasterizer: geometry for an evenly spaced, round dotted underline`.

### Task 6: DecorationRenderer draws the phased round dots

**Files:**
- Modify: `src/vtrasterizer/DecorationRenderer.hpp` / `.cpp`
- Test: `src/vtrasterizer/DecorationRenderer_test.cpp`

**Interfaces:**
- Consumes: Task 5.
- Produces:
  - `DirectMappedDecorationCount = std::numeric_limits<Decorator>::count() + MaxDottedUnderlinePitch - 1`. Phase 0 lives at `Decorator::DottedUnderline`'s own index. Phase `p >= 1` lives at index `count + p - 1`. `initializeDirectMapping()` uploads the phases `1 .. phaseCount-1` after the per-`Decorator` tiles, so `uploads[static_cast<size_t>(decoration)]` stays valid for the existing tests.
  - `void renderDecoration(Decorator decoration, crispy::Point pos, vtbackend::ColumnOffset firstColumn, vtbackend::ColumnCount columnCount, vtbackend::RGBColor const& color);` Column `i` draws the phase tile `dottedUnderlinePhaseOf(g, unbox(firstColumn) + i)` for `DottedUnderline`, and the single tile for every other decorator. `renderLine` passes `ColumnOffset(0)`. `renderCell` passes `ColumnOffset(unbox(cell.position.column) * scale)`.
  - The `DottedUnderline` case of `createTileData` paints `dottedUnderlineCoverage` (phase 0). A new private `createDottedPhaseTile(int phase, atlas::TileLocation)` paints the other phases.

- [ ] **Step 1: Write the failing tests and update the existing ones:**
  - `DecorationProbe::upload()`: relax the `REQUIRE` to `uploads.size() >= count` and add `uploadDottedPhase(int phase)`, which returns phase 0 → `upload(DottedUnderline)` and `p` → `uploads[count + p - 1]`.
  - Rewrite the squareness check in `dottedUnderlineSurvivesAShallowUnderlinePosition`: the first dot is as tall as it is wide (count the inked columns of the first contiguous run), `litPixels > 0`, and `topmostInkOffset == position`.
  - New `DecorationRenderer.dottedUnderlineContinuesAcrossCells`: Nimbus metrics, width 10 and thickness 1 (2 phases), plus width 9 (4 phases). Join the bitmap rows of phase tiles `c % phaseCount` for `c` in `0..7`. Dot starts in the darkest row must be spaced exactly `pitch` apart.
  - New `DecorationRenderer.dottedUnderlineDotsAreAntialiased`: with thickness 2, the tile has at least one pixel with coverage strictly between 0 and 255. Read the raw bitmap for this, not `bitmapRows`.
  - New `DecorationRenderer.dottedUnderlineContinuesOnADoubleWidthLine` (Review Focus 5): `renderCell` for columns 0 and 1 with `LineFlag::DoubleWidth` samples phase tiles `(0 * 2) % n, (0 * 2 + 1) % n, (2) % n, (3) % n`. Use the tile-location matching from `decorationsDrawnFor`, extended to the phase tiles.
  - `underlineFamilyStaysBelowTheBaseline` and `linkRole…` tests must keep passing unchanged.
- [ ] **Step 2: Run** `out/clang-asan/src/vtrasterizer/vtrasterizer_test "[decoration]"` → the new cases fail.
- [ ] **Step 3: Implement** the interfaces above.
- [ ] **Step 4: Run** `out/clang-asan/src/vtrasterizer/vtrasterizer_test` (whole binary) → pass.
- [ ] **Step 5: Commit** `vtrasterizer: draw the dotted underline as even round dots across cells`.

### Task 7: Release note and full verification

**Files:**
- Modify: `metainfo.xml` (the top `<release version="0.7.1" …>` list)

- [ ] **Step 1: Add two `<li>` entries.** First: "Adds `link_detection` to choose, per screen, whether URLs and file paths in output are detected; file paths are no longer detected on the alternate screen by default." Second: "Draws the dotted underline as evenly spaced round dots that continue across cells."
- [ ] **Step 2: Build** with `cmake --build --preset clang-asan` → no warnings, exit 0.
- [ ] **Step 3: Format check** with `git diff --name-only master -- '*.cpp' '*.hpp' | xargs clang-format --dry-run --Werror` → no output.
- [ ] **Step 4: Run** `ctest --preset=clang-asan` → 100% passed. Report the summary line.
- [ ] **Step 5: Run** `/simplify` on the branch diff and apply what it finds. Rebuild, then rerun the affected tests.
- [ ] **Step 6: Commit** `metainfo: release notes for per-screen link detection and the dotted underline`.
