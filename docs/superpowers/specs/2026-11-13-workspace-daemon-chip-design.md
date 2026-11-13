# One chip for daemon and workspace — design

Board: “First-run discoverability · workspaces and daemons”, proposal **A** (marked chosen), on the Contour
Native Design canvas (<https://claude.ai/artifact/RcxAEXjArUvKdqXdWdGTyu>, `chip-discovery.dc.html`).

## Intent

A user who has just launched Contour in daemon mode should be able to find, **without prior knowledge**:

1. that the window shows one of several **workspaces**, and how to switch between them;
2. that the workspaces live in a **daemon**, that several local daemons can run, and that remote daemons
   can be attached;
3. the **keyboard shortcut** for all of the above.

Today none of this is visible. With the sidebar hidden, the workspace chip (`WorkspaceChip.qml`) names only
the workspace, its tooltip says “Switch workspace” with no shortcut, and its dropdown lists only this
daemon's workspaces. The host chip (`HostChip.qml`) is hidden for the default local daemon, because
`DaemonLink::chromeLabel()` returns nothing for the plain-launch daemon. The sidebar never names a daemon.
The shortcut that reaches everything, `ShowSwitcher` (⇧⌘R, or Ctrl+Shift+R), is found only by searching the
command palette for “Switcher”.

The owner found this while checking whether the first-run UI/UX is right for new users.

## Goals

- In daemon mode the title band always shows **where the session runs and which workspace it is**, as one
  chip: `local › billing ▾`, `work › billing ▾`, `● starfive › billing ▾`.
- One click on the chip and one shortcut (the existing `ShowSwitcher`) open **the same popup**. The chip's
  tooltip names the shortcut, so clicking the chip teaches the key.
- When the sidebar is shown, the same daemon control heads the sidebar, so the daemon stays visible in both
  layouts.
- The host chip's connection states (connected, *Reconnecting…*, *Disconnected*) survive inside the merged
  chip.

## Non-goals

- No new action and no new default chord. (Proposal B needed one, and every plausible chord collides with a
  shell, editor or tmux key, as `BindingCollisions_test.cpp` records.)
- No change to the switcher's rows, levels, filtering or keys beyond the scope it opens with.
- In-process windows (no daemon) are not redesigned. They keep a workspace-only chip, as today.
- The four bugs found in the same session are being fixed on the sibling branch
  `fix/sidebar-focus-and-workspace-cycle`, which lands first:
  - CycleWorkspace on Ctrl+Cmd+↓ does nothing on macOS.
  - Enter on a sidebar row does not return keyboard focus to that tab's pane.
  - Switching workspaces does not restore the window's last-focused tab and pane in that workspace.
  - Clicking the workspace chip in an in-process window does nothing.

## Anti-goals

The design has failed, even if it works, if:

- **The workspace name gets truncated before the daemon name.** The workspace is what the user works in;
  the daemon part shrinks first.
- **The chip pushes the tabs out of the band** at ordinary window widths.
- **The click and the shortcut open visibly different popups.** That is proposal C's flaw, which A exists
  to avoid.
- **Connection state is carried by colour alone.** The dot keeps its shape, and the state text stays
  available to the tooltip and the screen reader even when it is elided.

## Approaches considered

- **A · One chip for both (chosen).** The workspace chip gains a daemon segment and absorbs the host chip.
  A click opens the full switcher (`SwitcherScope::Everything`) hanging below the chip. Its tooltip shows
  ⇧⌘R. When the sidebar is shown, the daemon part heads the sidebar.
  - *Trade-offs:* the chip is wider, and the workspace-only dropdown goes away.
- **B · Two chips, always shown.** Show the host chip for the default local daemon too, keep the
  workspace-only dropdown, and add a new `ShowWorkspaceSwitcher` action with its own default chord.
  - *Rejected:* it needs a free chord, gives two shortcuts to learn, and puts two chips in the bar.
- **C · The bar stays, the hints move in.** Add the shortcut to the tooltip, add a “Daemons and Hosts…” row
  to the dropdown, and add a daemon heading to the sidebar.
  - *Rejected:* with the sidebar hidden, daemons are never on screen. The tooltip's key would also open a
    differently placed popup than the click does.

The owner picked A on 2026-11-13, after a UX comparison: it is visible without interaction, has one control
and one key, reads as a location (`daemon › workspace › tab`), and declutters the bar.

## The design

### The chip

- **Label.** `<daemon> › <workspace> ▾`. The daemon segment is secondary text; the workspace is the primary,
  semibold label, as today.
  - The default local daemon reads **`local`** (*my call:* its socket label is `default`, which tells a new
    user nothing).
  - A named local daemon reads its label (`work`).
  - A remote daemon reads its host label, as the host chip does today.
  - A daemon with no name (`SwitchDaemonKind::Same`: `--connect-tcp`, `--socket`, `$CONTOUR_MUX`) reads
    `daemon`. *My call:* the switcher's “this daemon” reads oddly in a chip.
  - The popup uses the same names as the chip, so under `local › billing` its group reads “Workspaces on
    local”, not “Workspaces on default”. `switcherDaemonName` and the chip share one naming function.
- **Connection state.** Every daemon link reports its connection, local ones included: a local daemon is
  *Reconnecting* during a live upgrade and *Disconnected* once stopped, as today's `work` chip already shows.
  - While connected, a remote daemon shows a green dot; a local daemon shows no dot, to keep the everyday
    case quiet.
  - While not connected, any daemon shows the dot and the state text (`starfive Reconnecting…`,
    `local Disconnected`).
  - The look of each state stays `UiStyleProvider::hostChipLook`'s table.
  - *My call:* which band notices (`HostNotice::Interrupted`, the pinned full-screen band) the default daemon
    raises does not change; it already reports its connection. Only what the chip draws changes.
  - “Remote” means `DaemonLocality::Remote`.
- **Shown when.** The owner decided (2026-11-13): **in daemon mode, every visible title band shows the full
  chip at its leading edge**, including on the default local daemon. This reverses the `chromeLabel()` rule
  *for the chip only*; the window title (`RemoteChrome`) keeps its current rule. Per band layout
  (`BarCentre`, `TitleBandLayout.hpp`):
  - **`ChipAndTabs`** (tabs in the sidebar, sidebar hidden): the chip leads the tabs, as today's workspace
    chip does.
  - **`Tabs`** (tabs at the top, the Classic edition): the chip leads the tabs too. Today this band carries
    only the host chip, and no workspace chip at all.
  - **`Title`** and **`Empty`** (one tab; Minimal's bare band): the chip sits at the leading edge, and the
    title stays centred.
  - **`Breadcrumb`** (sidebar shown): no chip in the band; the daemon control heads the sidebar (below).
  - **Hidden band** (flat or native frame, strip gate Hidden): nothing, as today.

  The `TitleBandRules` change follows from this: the host chip's slot becomes the merged chip's slot on
  every band, and `ChipAndTabs` stops being the only centre with a workspace control.
  - **In-process window:** the workspace segment alone, as today. Its tooltip stays “Switch workspace”
    with the ShowSwitcher chord. Today a click on that chip is a logged no-op (`openSwitcherOver` returns at
    `logNoDaemon`). That bug is being fixed on `fix/sidebar-focus-and-workspace-cycle`, so this design
    assumes a working in-process workspace picker and does not rebuild it.
- **Width.** Elision order, applied as the band runs short:
  1. drop the state text (keep the dot);
  2. elide the daemon name in the middle;
  3. drop the daemon segment entirely, except for the dot;
  4. only then elide the workspace name, down to a floor of 6 characters (cells) plus `…`.

  *My call:* the chip starts eliding before the tabs would drop below their minimum width, so it yields
  before the strip goes icon-only or scrolls (`tabWellLayout`'s sequence). When the state text has been
  elided, the tooltip prepends it: “Reconnecting to starfive · Switch workspace or daemon ⇧⌘R”.

  The band's existing guarantees (the minimum draggable gap, tab minimum widths) are kept; the builder fits
  the chip into `TitleBar.qml`'s width budget, where `hostChipSlot` and `workspaceChipSlot` are summed today.
- **Click / Space / Return.** Opens the switcher with `SwitcherScope::Everything`, anchored below the chip
  (`SwitcherOrigin::Chip`). The workspaces of the window's own daemon come first, and typing filters
  immediately.
  - The initial selection stays what `switcherInitialSelection` does today: the first workspace this window
    does *not* show, so Return toggles to another workspace.
  - Escape closes the popup. Today a chip-anchored popup with `Everything` scope counts as *widened*:
    `isNested()` is true and `SwitcherController::back()` narrows it to `OneDaemon` instead of closing. That
    widened state is removed along with the chip's `OneDaemon` path. This is required, not optional, because
    a popup that narrows on Escape would look different from the one ⇧⌘R opens.
- **Tooltip.** “Switch workspace or daemon”, with the `ShowSwitcher` chord through `KitToolTipHost`'s
  `shortcutCommand`, as `HostChip.qml` already does. It shows whatever chord the user's config binds; with
  none bound, no chord is shown.
- **Accessibility.** Follows ui-ux rule 8.2, which puts status in the description, not the name. *My call:*
  this chip follows the rule, although both chips it replaces break it.
  - Name: “Workspace billing on starfive”.
  - Description: the state, if any, then expanded/collapsed: “Reconnecting, collapsed”.
  - Role stays `ButtonMenu`.
  - The status glyph marker (`workspaceChipMarked`) is kept.
- **Terminal style (`ui_style: terminal`).** Drawn on the board beside the native look, for every situation:
  - **Label.** Bracketed cells, `[local › billing ▾]`, with the workspace marker after the `▾` as today
    (`[local › billing ▾ !]`).
  - **Colours.** The daemon segment uses the scheme's bright black; the workspace uses the foreground, bold.
  - **Connection state.** A glyph, never a fill: `●` connected (green), `◑` reconnecting (yellow glyph),
    `✕` disconnected (red), e.g. `[◑ starfive Reconnecting… › billing ▾]`. The shapes differ, so colour is
    never the only cue.
  - **Width.** Elision runs in whole cells. The brackets, the state glyph and `▾` never elide.
  - **Popup.** The switcher is the square, in-scene popup with the hard one-cell shadow.
  - **Sidebar shown.** The daemon control is the first row of the cell tree: `[local ▾]`.

### Shortcut

`ShowSwitcher` keeps its binding (⇧⌘R / Ctrl+Shift+R) and keeps opening **centred**. It and the chip open the
same scope and rows; only the anchor differs, which is fine, because the shortcut has no control to hang
from.

### Sidebar shown

- The title band's centre stays the breadcrumb (`workspace › tab`).
- The sidebar's title row gains the daemon control at its head: `local ▾`, with the dot for a remote daemon.
  It opens the same switcher, anchored below itself, and carries the same tooltip.
  - On the Linux header pane, the title row is part of the header bar (`TitleBar.qml`). The control goes
    there.
- The host chip no longer appears in the band in this layout either, so the daemon is named in exactly one
  place in each layout.

### What goes away

- `HostChip.qml` as a separate band item. Its look, states and `hostChipLook` table move into the merged
  chip.
  - *My call:* fold it into `WorkspaceChip.qml` (or rename that to `LocationChip`) rather than keep two
    components side by side. The builder may keep `HostChip` as an inner part if that is cleaner.
- The chip's workspace-only dropdown entry point: `showWorkspaceDropdown` opening `SwitcherScope::OneDaemon`,
  and its “All daemons…” widen row.
  - *Note:* `OneDaemon` is also used when a switch asks to pick a workspace of a given daemon. That use
    stays; only the chip's use and the widen path go, if nothing else needs them.

## Constraints

- **Placement.** The Qt-free decisions go into the `window/` headers that already hold them (label
  composition, elision order, accessible name), table-tested in the module's tests. QML lays parts out and
  decides nothing (see `.agent/rules/ui-ux.md`).
- **Styling.** Every size, colour and glyph comes from `chromeStyle`; no literals.
- **Platforms.** All four chrome rows are covered: macOS, Fluent, ContourLinux and Terminal.
- **Tests.** The existing GUI and accessibility tests that name `hostChip` or `workspaceChip`
  (`Accessibility_qml_test.cpp`, the title-bar tests) are updated rather than deleted. Offscreen grabs of the
  chip are compared with the board before the work is called done.
- **Build.** Zero warnings, and `ctest --preset=clang-asan` green.

## Left to the builder

- The exact component split, and where the new pure helpers live in `window/`.
- Whether the merged chip keeps `objectName: "workspaceChip"`, or renames it and updates the tests.
- How the sidebar title row fits the control on each chrome row. The board shows macOS; the others follow
  the same model.
- How to delete `SwitcherRowKind::AllDaemons` and the widened-chip state in `SwitcherController` and
  `SwitcherKeys`, keeping `SwitcherScope::OneDaemon` for the switch-asks-to-pick path.
