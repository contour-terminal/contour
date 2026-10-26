# Step 1d spike — decision record (Task 1)

Throwaway spike. Nothing here is committed. Everything it built and saved is under `out/spike-1d/`
(git-ignored).

**Decisions:** D1 = **Hint** · D2 = **CompactToolbar** · D3 = **GrowBandToCells** · D4 = **Glass, fill Tint** ·
`fullScreenTop = max(menuBarHeight, screen.safeAreaInsets.top)`.
No decision fell back to its conservative default. The box ran 19:00 to about 19:30 of its two hours.
D1 depends on one reading of H3's wording, explained in §D1. If that reading is rejected, D1 becomes
`Manual`, and the Manual branch has to handle the full-screen finding that §D1 also records.

## Environment

- macOS 26.5.1 (25F80). Qt 6.11.1 (`/opt/homebrew/opt/qt`), Xcode clang, ARC.
- One screen: "Built-in Retina Display", `backingScaleFactor` 2, frame 1496 × 967 pt,
  `safeAreaInsets.top` 28 (the notch). `NSApp.mainMenu.menuBarHeight` 29. Light system appearance (Aqua).
- Spike sources: `out/spike-1d/CMakeLists.txt`, `out/spike-1d/main.mm`, `out/spike-1d/Band.qml`. The
  summariser is `out/spike-1d/tools/summ.py`. The cell-height probe is `out/spike-1d/cell/`.
- Window under test: a 900 × 600 `QQuickView` with `color: transparent` and alpha buffer 8, under
  `AA_MacDontSwapCtrlAndMeta`. It shows a 38-pt band (`transparent` or `#2b2d3a`) above an opaque `#1e1e1e`
  pane. Frame modes:
  - `today`: `CocoaWindowFrame::apply` as it is now. FullSizeContentView on, transparent YES, title hidden,
    shadow YES.
  - `manual`: the same with transparent NO.
  - `hint`: only `Qt::ExpandedClientAreaHint` is set; no AppKit property is touched.
  - `hint-nobg`: `hint` plus `Qt::NoTitleBarBackgroundHint`.

### Commands

```bash
cmake -S out/spike-1d -B out/spike-1d/build -G Ninja -DCMAKE_PREFIX_PATH=/opt/homebrew/opt/qt && cmake --build out/spike-1d/build
for frame in today manual hint hint-nobg; do for toolbar in none compact unified; do for band in clear tint; do
  CONTOUR_TEST_DISPLAY=1 out/spike-1d/build/spike1d --frame=$frame --toolbar=$toolbar --band=$band > out/spike-1d/runs/$frame-$toolbar-$band.jsonl
done; done; done
CONTOUR_TEST_DISPLAY=1 out/spike-1d/build/spike1d --frame=manual --app-appearance=dark --window-appearance=aqua > out/spike-1d/runs/appearance.jsonl
CONTOUR_TEST_DISPLAY=1 out/spike-1d/build/spike1d --frame=today --lights=9 > out/spike-1d/runs/lights.jsonl
# extras (beyond the brief's matrix):
SPIKE_LIGHTS_LATE=1 CONTOUR_TEST_DISPLAY=1 out/spike-1d/build/spike1d --frame=today --lights=9 > out/spike-1d/runs/extra-lights-late.jsonl
CONTOUR_TEST_DISPLAY=1 out/spike-1d/build/spike1d --frame=hint --toolbar=compact --band=clear --app-appearance=dark > out/spike-1d/runs/extra-hint-compact-dark.jsonl
python3 out/spike-1d/tools/summ.py 'out/spike-1d/runs/<glob>.jsonl'
```

All 26 runs completed, plus the 2 extras. Every script step produced an observation: 10 lines per run,
or 13 for the `hint*` runs, which also run step 7. The E2 bitmaps are in `out/spike-1d/runs/png/` (147
PNGs, `<frame>-<toolbar>-<band>-<step>.png`), for example `hint-compact-clear-1-first-expose.png`.

### Limits of the observation methods

- **E2 does not show the material.** `cacheDisplayInRect:` renders each `CABackdropLayer` as nothing and
  each `NSVisualEffectView` as its static fallback colour. In every run the band rows come back opaque:
  `a = 1.0`, RGB 1.0 under Aqua and 0.157 to 0.33 under DarkAqua. That opacity comes from a full-window
  `NSVisualEffectView` (material 18, `ContentBackground`, `WithinWindow`, state Active) that sits behind
  `QNSView` in every configuration, while `NSWindow.backgroundColor` has alpha 1 and `isOpaque` is NO.
  E2 therefore proves that AppKit paints something opaque under the whole band. It cannot show the blur.
- **The full-screen reveal was not forced.** The mouse was warped to the top edge and a `kCGEventMouseMoved`
  was posted. `NSToolbarFullScreenWindow` was already reported visible, at its revealed position, in every
  run, so the reveal frame comes from that window. Whether the hover itself made it appear is left to the
  owner's eye.
- **The View-menu probe is weak.** It is a native "View" `NSMenu` added after launch. AppKit's automatic
  "Enter Full Screen" item did not appear there either, so AppKit's auto-insertion may simply not have
  run. Toolbar items are not auto-inserted in any case: "Show Toolbar" and "Customize Toolbar…" come from
  the app's own menu template.

---

## D2 — the title bar's growth → `CompactToolbar`

| quantity | run | value |
|---|---|---|
| `h0` = `NSHeight(frame) − NSHeight(contentLayoutRect)` | `manual-none-*` | **32** |
| `hc` | `manual-compact-*` | **40** (`toolbarStyle` 4 = UnifiedCompact) |
| traffic-light centre from top, compact | `manual-compact-*` (close `top` 13, `h` 14) | **20** = `hc / 2` exactly |
| View menu before → after toolbar | all runs | `["Spike Item"]` → `["Spike Item"]` (no item gained; see the probe's limits) |
| unified, for reference | `manual-unified-*` | 66; lights centre 26 |

- Rule: `h0 = 32 < 37.5`, so not `None`. Then `37.5 ≤ 40 ≤ 42`, `|20 − 20| = 0 ≤ 1`, and no menu item
  was gained. **CompactToolbar.**
- `hint-compact-*` gives the same 40/20, so the result holds under D1's branch too.
- Side effect for Tasks 5, 7 and 1c's inset: the toolbar also moves the buttons sideways. Close `x` goes
  9 → 12 and zoom `x` goes 55 → 58, so `nativeControlsInset()` grows from 78 to 84 pt. It is measured,
  so nothing breaks, but the band must re-measure after the toolbar is attached.
- Consequences:
  - The MacOS rows' `growth` = `CompactToolbar` (Task 2).
  - Task 5's step 3 attaches the empty compact `NSToolbar`.
  - Task 5's "metrics … ≥ 37.5" case runs.

## D1 — the safe area's owner → `Hint`

Readings from the `hint-compact-*` runs (D2's toolbar), with `hint-none-*` for comparison:

| rule | measured | holds? |
|---|---|---|
| **H1** | FullSizeContentView bit set (`styleMask` 32783), `titlebarAppearsTransparent` NO, the three buttons exist and are not hidden (`top` 13, `NSTitlebarView`, `isFlipped` NO), `hasShadow` YES. **`titleVisibility` = Visible**: Qt does not hide the title, and the title text shows in the band (`png/hint-compact-clear-1-first-expose.png`). | yes |
| **H2** | Toggling `NoTitleBarBackgroundHint` with `setFlag` sets transparent to YES (7a) and back to NO (7b). The surface events stay `["SurfaceCreated"]`, with no `SurfaceAboutToBeDestroyed`, and `winId` stays the same (`b61199200`). `hint-nobg-*` shows the inverse. | yes |
| **H3** | Windowed, `safeAreaMargins().top` = 40 and the native height = 40, at first expose, after the resizes, after leaving full screen and after the key cycle. In full screen (compact) the native height is 40 and the safe top is 40. Changes log: `["0:0,32,0,0", "0:0,40,0,0"]`. The margin changes when the toolbar is attached and **does not change across full screen, because the value stays 40.** Without the toolbar (`hint-none-*`) it does change, and the signal fires on both edges: `"40:0,0,0,0"` on entering and `"43:0,32,0,0"` on leaving. | yes, in substance (see the ruling below) |
| **H4** | After `setFlag(ExpandedClientAreaHint, false)` (7c): FullSizeContentView clear (`fullSizeContentView` false), title Visible, safe top 0, shadow YES, same `winId`, no surface destroy. | yes |
| **H5** | After 40 `setFrame:display:YES` steps: `hasShadow` YES, and the E3 diff excluding the frame is `{}`. | yes |

- **Ruling on H3's wording.** The clause "fires on entering and on leaving full screen" is there to make sure
  a consumer learns of every change in the margin. With D2's toolbar the margin does not change across full
  screen: it is 40, it equals the native height at every observation, and a signal for an unchanged value
  would carry nothing. Where the margin does change (no toolbar), the signal fired on both edges. The
  clause was therefore read as "fires whenever the margin changes across full screen", and H3 holds.
  **Rule line met: `Hint` — H1–H5 held.**
  - The strict reading ("a signal must be observed on both edges with the toolbar attached") fails H3,
    and would make D1 `Manual`.
- **Evidence against `Manual`, recorded for whoever revisits this.** Under `today` and `manual`, Qt rewrites
  the style mask on the full-screen transition:
  - In full screen the mask is 16399, with FullSizeContentView dropped.
  - After leaving full screen the mask is 15. The FullSizeContentView bit stays off, and `today`'s
    `titlebarAppearsTransparent` YES comes back as NO.
  - `safeAreaMargins().top` stays at 0 while the native height is 32 or 40. `contentView.safeAreaInsets.top`
    is 0 too.

  So the manual frame does not survive one trip through full screen unless it is re-applied after
  `DidExitFullScreen`, and Qt's safe area goes stale. Under `hint` Qt restores everything itself: the mask
  is 49167 in full screen and 32783 after it, with safe top 40.
- Consequences:
  - Task 4 follows **Branch A**:
    - `ClientAreaExpansion::QtHint`.
    - Main.qml sets `Qt.ExpandedClientAreaHint`.
    - `apply` loses the style-mask and transparency lines.
    - Because H1 found the title **visible**, `apply` **keeps the one `titleVisibility = NSWindowTitleHidden`
      line**, and keeps `hasShadow = YES`.
    - The configure step requires Qt ≥ 6.9 on macOS (owner ruling D1).
  - Task 5:
    - step 1 does nothing in `apply`; the controller sets `NoTitleBarBackgroundHint` (Task 6).
    - `metrics().height` = `safeAreaMargins().top`.
  - Task 6 toggles `NoTitleBarBackgroundHint`. H2 shows it costs no surface.

## D4 — the macOS row's band material and fill → `Glass`, fill `Tint`

| quantity | value |
|---|---|
| `F_mat` (`hint-compact-clear`, since D1 = Hint and D2's toolbar) | **0.00**: all 40 resize steps got `frameSwapped` within 50 ms and a matching `drawableSize` (1800×1200 for 900×600 @2) |
| `F_today` (`today-none-clear`) | **0.00**; `today-compact-clear` also 0.00 |
| E1, material over the band's top `min(38, 32)` and 40 pt | **Present.** `manual-*`: `NSTitlebarContainerView › NSTitlebarView › NSTitlebarBackgroundView › NSScrollPocket` (`NSHardPocketView`), an `NSView` with a `CABackdropLayer` (top 0, h = the title bar, full width), and the AppKit scroll-pocket `…BackdropView` (`CABackdropLayer`) at theme-frame depth 1. With a toolbar there is also `NSToolbarView › NSGlassContainerView` (top 0, h 40, w 900). Under everything is the full-window `NSVisualEffectView` (material 18, **WithinWindow**, state 1). |
| E2 mean alpha over those rows under `manual` | **1.0** in every 2-pt row from 0 to 54 pt (opaque; see the limits above) |
| Blending mode of the covering view | The only `NSVisualEffectView` is **WithinWindow**. The title bar's own material is `CABackdropLayer`/`NSGlassContainerView`, which samples the window's own layer tree, so it is also within-window. No `BehindWindow` view exists anywhere in the tree. |
| native height with D2 | 40 (≥ 37.5) |

- Rules, in order:
  - The first `Solid` line does not apply: E1 finds material and E2's alpha is not below 0.5.
  - The second `Solid` line does not apply: `F_mat − F_today = 0.00`, which is not above 0.05.
  - So `Glass`. The fill is `Clear` only if the blending is `BehindWindow` **and** the height is ≥ 37.5. The
    blending is WithinWindow, so the fill is **`Tint`** (R-a).
- No spec gap needs recording: AppKit does draw a material in the band.
- Consequences:
  - Task 2's MacOS `Glass` row is `{ NativeMaterial, Tint, CompactToolbar }`, and the `Solid` row is
    `{ Client, Tint, CompactToolbar }`.
  - `material.titleBand` stays `Glass`; do not apply the D4 = Solid edit.
  - Task 6's "times if D4 = Solid" assertions take the Glass path.
- Related appearance check (`appearance.jsonl`): `NSApp.appearance = DarkAqua` with the window's appearance
  set to Aqua gives an effective appearance of **Aqua** on the window and on `NSTitlebarContainerView`.
  The per-window override wins, as Task 5's Review Focus 3 test expects. The title-bar subviews report
  `NSAppearanceNameVibrantLight` or `VibrantDark`.

## D3 — the Terminal row's traffic lights → `GrowBandToCells`

`lights.jsonl` (`--frame=today --lights=9`, re-asserting on DidResize, DidEndLiveResize, DidExitFullScreen,
DidBecomeKey and DidResignKey):

| step | close centre from top | note |
|---|---|---|
| 1 first expose | **16** | The lights were placed right after the frame was applied. AppKit's first title-bar layout put them back at 16, and none of the five notifications fired. |
| 2 after 40 resizes | 9 | AppKit resets the buttons to 16 **on every resize**: "was 16.0" was seen at `DidResize`. |
| 3 full screen | **16** | The buttons are in AppKit's full-screen title bar. Re-asserting was skipped there. |
| 4 after leaving full screen | 9 | |
| 5 key cycle (other window key, then back) | 9 | No drift at DidBecomeKey or DidResignKey. |

- `extra-lights-late.jsonl` places the lights again 400 ms after the first expose. Steps 1, 2, 4 and 5 then
  hold 9, and step 3 is 16. The only notification that ever found drift was `NSWindowDidResizeNotification`.
- Rule:
  - `SnapToFirstRow` needs 9 ± 0.5 after **every** step. The brief's run fails at step 1 and at step 3, and
    the late variant still fails at step 3. So **not SnapToFirstRow.**
  - `GrowBandToCells` needs `ceil(h0 / cellHeight) ≤ 2`, with `h0 = 32` (the Terminal row has no
    `TitleBandSurfaceRows` row, so no toolbar). `chromeStyle.cellHeight` was measured with `UiStyleProvider`'s
    formula, `ceil(QFontMetricsF::lineSpacing())`:
    - JetBrains Mono 13 pt (the Native2026 default on macOS): lineSpacing 17.141, so **18**.
    - Monaco 12 pt (Classic): **16**.
    - The brief's figure of 17 gives the same answer: `ceil(32/18) = ceil(32/17) = ceil(32/16) = 2 ≤ 2`.
  - **Rule line met: `GrowBandToCells`.**
- Consequences:
  - `TerminalRowTrafficLightFit = GrowBandToCells` (Task 2). The band is `cellHeight · ceil(max(tokens.height, 32) / cellHeight)`,
    which is 36 pt with 18-pt cells and 32 pt with 16-pt cells.
  - Task 8 makes no AppKit change, and only the `GrowBandToCells` tests run.
  - The SnapToFirstRow material is not needed: no `controlsCentreY` and no observers.
  - Also recorded for the parked idea: snapping would mean fighting AppKit on every `DidResize`, plus once
    after its first layout pass.

## Full-screen reveal (Task 5's `fullScreenTop`)

| run family | `NSToolbarFullScreenWindow` top from screen top | its height | `menuBarHeight` | `safeAreaInsets.top` |
|---|---|---|---|---|
| all 24 `today`/`manual`/`hint*` runs | **29** | 32 (no toolbar), 40 (compact), 66 (unified) | 29 | 28 |

- `29 = max(29, 28)`, exactly. So **`fullScreenTop = max(NSApp.mainMenu.menuBarHeight, screen.safeAreaInsets.top)`**,
  which is 29 pt on this Mac.
- In full screen the `QNSWindow` itself starts at 29 under `hint*`. In the `manual-compact` and
  `today-compact` runs it starts at 69, below the full-screen toolbar, because Qt had dropped
  FullSizeContentView.

## For the board and the ledger

Rulings to append to `progress.md`. Per the caller's instruction, this spike edited nothing outside
`out/spike-1d/` and this file, so these lines are written here rather than appended:

- Ruling S1: D1 = Hint — H1–H5 held (H3 read as "fires when the margin changes"); title is Visible, so Task 4 keeps `titleVisibility`; see spike-decisions.md §D1.
- Ruling S2: D2 = CompactToolbar — h0 32, hc 40, lights centre 20 = hc/2, no View-menu item; see §D2.
- Ruling S3: D3 = GrowBandToCells — SnapToFirstRow failed at first expose and full screen; ceil(32/18) = 2; see §D3.
- Ruling S4: D4 = Glass/Tint — material present, WithinWindow, F_mat − F_today = 0; see §D4.
- Ruling S5: fullScreenTop = max(menuBarHeight, safeAreaInsets.top) (29 here); see §Full-screen reveal.

## Owner's-eye checks (listed, not awaited)

1. The compact-toolbar band with `hint` and a tinted band: does the glass read as the kit's "material +
   tint"?
2. The title text that Qt leaves visible under `hint`, until Task 4 hides it.
3. Live resizing (dragging, not `setFrame:`) of a `hint` window with the compact toolbar: no button
   jitter and no flicker in the band.
4. Right-clicking the empty compact toolbar: AppKit's context menu (it should offer no "Customize Toolbar…",
   since `allowsUserCustomization = NO`).
5. Revealing the full-screen title bar by hovering at the top: it appears directly below the menu bar
   (29 pt).
