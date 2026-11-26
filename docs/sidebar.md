# The workspace sidebar

The sidebar is a list down the left edge of a window. It shows every workspace of the daemon the window is
attached to (or the window's own workspaces, when sessions run in-process) as a tree: a **workspace**, the
**tabs** in it, and the **panes** of a tab that is split. It is also an inbox: it tells you which program
is waiting for you, which finished while you were looking elsewhere, and takes you there with one key.

!!! note
    The sidebar is part of the `native2026` defaults. A new configuration shows it; a configuration written
    before the redesign (`defaults_edition: classic`) keeps its tab strip and hides it. See
    [Defaults](#defaults).

## What a row shows

A workspace row is one line. A tab or pane row is always two lines tall, so the list never jumps as a program
starts or stops talking: a status glyph, a title and a time on the right, and beneath them a second line. The
second line is the reason when the program has something to say, and otherwise the **idle line**: the folder
the pane is in and the program running there, such as `~/src/billing · claude`, either one left out when it is
not known.

| Row | Title | Right-hand side | Second line |
| --- | --- | --- | --- |
| Workspace | Its name, or the folder or repository it was started in | The git branch with `↑` commits ahead and `↓` behind its upstream, such as `feat/billing ↑2 ↓1`, then the status glyph of its most urgent pane | None |
| Tab | Its name, or the title its program or the title template gives it | Time in the current state | What the program in it says it is doing; otherwise its folder and program |
| Pane | Shown only for a tab with more than one pane | The same | The same |

A tab's rows list in the order of the tab strip, and follow it when a tab is moved there.

- **Status and reason.** The status comes from agent hooks (see `contour agent setup`), from a blocked
  program, from OSC 133 shell integration (a command that finished, with its exit code) and from the bell. The
  reason is that program's own words (`waiting for approval`), or how the command ended (`exit 1 · 18s`). A
  command a program asks permission to run (`Bash: psql $DEV_URL`), or the command line of a finished one
  (`exit 1 · make deploy`), is set in the monospace face; the words before it (`Permission · `, `exit 1 · `)
  stay in the proportional face, on the same baseline. Reason text is shown as plain text, one bounded line,
  whatever the program sent.
- **Time.** A running state counts up (`4:12`), a finished one counts since it ended (`2m ago`), and a program
  that reports progress shows a percentage meter instead. The clock ticks once a second, and only while a
  visible row shows a live time. A finished command's exit status has no timestamp to measure from, so it has
  a reason but no time.
- **Roll-up.** A parent row shows its most urgent descendant: the glyph, reason and time of the pane that
  asks the most. A collapsed workspace therefore still tells you that something inside it needs you.
- **Git.** A workspace row shows the branch of its active pane's repository, with `↑` and `↓` once an
  upstream exists. A directory outside a repository, a detached `HEAD` and a branch without an upstream
  show only what is known. The daemon refreshes it when the active pane changes directory, when the active
  tab or pane changes, when a command finishes (OSC 133;D) and every 30 seconds, whether or not a window is
  attached, so another daemon's switcher rows show it too. A window without a daemon refreshes on the same
  events but has no 30-second timer. The probe never blocks the daemon or the window.
- **Current tab.** The tab this window displays is marked as selected, as the platform marks it: on macOS in
  the accent while the keyboard is in the sidebar and in a quiet grey while it is elsewhere, as a source
  list's selection is; on Linux a neutral fill, focused or not, with the accent focus ring on the row the
  keyboard is on; on Windows a subtle grey fill and a rounded accent pill at the leading edge, focused or not;
  on the Terminal style inverse video. When its workspace is folded, the workspace row carries the selection
  instead, until it is unfolded.
- **Failed.** A failed status is drawn as the red ❌ on the native rows, in the sidebar, the location chip and
  the tab strip's badge; the Terminal style keeps a single-cell red ✕, because ❌ is two cells wide.

## Urgency

One order ranks every status, in the sidebar, the tab strip, the taskbar and the location chip, most urgent
first:

| Rank | Urgency | Meaning |
| --- | --- | --- |
| 1 | **Needs you** | A program is blocked waiting for you, or a pane rang the bell |
| 2 | **Failed** | A command or agent failed and you have not seen it |
| 3 | **Done, not seen** | Something finished and you have not seen the result |
| 4 | **Working** | Something runs; it needs nothing yet |
| 5 | **Unseen output** | Output arrived out of sight |
| 6 | Idle | Nothing to show |

Each status has its own shape as well as its colour, so none relies on colour alone. Working uses the accent
colour, and needs-you uses amber (on the Terminal style, as a glyph only). A done or failed result stays
shown until you see its pane, which is the *done, not seen* state. Seen never hides a program that is still
blocked.

## Jump to the next item that needs you

**⌘J** (macOS) or **Ctrl+Shift+J** (Linux and Windows), the `JumpToNextAttention` action, shows the next pane
that asks something of you. It visits needs-you panes first, then failed, then done-not-seen, each class in
sidebar order, and wraps around. It never lands on a working or idle pane. Landing on a pane marks it seen.
**Mark Seen** in a row's menu (the `MarkSeen` action) does that without going there.

The footer shows the key, so you find it again.

## Showing, hiding and focusing

| Action | macOS | Linux and Windows |
| --- | --- | --- |
| `ToggleSidebar`: show or hide | ⌘\ | Ctrl+Shift+B |
| `FocusSidebar`: move keyboard focus into it, showing it first when hidden | ⌘⇧E | Ctrl+Shift+D |
| `JumpToNextAttention` | ⌘J | Ctrl+Shift+J |
| `CycleWorkspace`: show the next / previous workspace | ⌃⌘↓ / ⌃⌘↑ | Ctrl+Alt+PageDown / PageUp |
| `MoveWorkspaceDown` / `MoveWorkspaceUp`: move the current workspace in the list | ⌃⇧⌘↓ / ⌃⇧⌘↑ | Ctrl+Shift+Alt+PageDown / PageUp |
| `NewWorkspace`: create a workspace and show it | ⇧⌘N | Ctrl+Shift+Alt+N |
| `FilterWorkspaces`: open the filter, showing the sidebar first when hidden | none | none |
| `ToggleSidebarNeedsMe`: list only what needs you, or everything again | none | none |

These are bound in the `native2026` defaults only, except `CycleWorkspace`, `MoveWorkspaceDown`/`Up` and
`NewWorkspace`, which every defaults edition binds, one level up from the tab and window chords (Ctrl+PageUp/PageDown,
Ctrl+Shift+PageUp/PageDown, Ctrl+Shift+N). `FilterWorkspaces`
and `ToggleSidebarNeedsMe` have no default chord; bind one in `input_mapping`. Each control's tooltip names the chord its action is bound to at the time, so a
binding you add shows there at once.

By pointer, the **sidebar toggle** at the start of the title band (beside the traffic lights on macOS, at the
header bar's start on Linux, in the sidebar's own top row while a macOS sidebar is shown) hides and shows it,
from the same spot both ways. The title bar's menu has **Show Sidebar** / **Hide Sidebar**, and so has the View
menu on macOS. When no title band is on screen (full screen, or `tab_bar: never` with the title bar hidden) and
the sidebar is hidden, the terminal's own right-click menu offers **Show Sidebar**, so the sidebar can always
be brought back the way it went.

**Needs me.** The sidebar's Needs-me control (beside the filter button; on Linux, whose header bar above the
sidebar has no room for it, beside the filter field, which stays open while the tree is narrowed), the `ToggleSidebarNeedsMe` action and **View ▸ Show Only What Needs Me** narrow the tree to the rows on
the way to a pane that needs you (blocked, failed, or done and not seen), plus the tab the window shows and a jump
under way. A second press of the same control lists everything again, folds as they were. With nothing to show the
tree says **Nothing needs you** and offers **Show all**. The width you drag it to and whether you left it shown are
remembered per user and win over the configured start state.

## Keyboard

With the focus in the sidebar (**⌘⇧E** or **Ctrl+Shift+D**):

| Key | Does |
| --- | --- |
| ↑ / ↓ | Move to the previous or next row |
| Home / End | Move to the first or last row |
| ← | Collapse the row; on a collapsed or leaf row, move to its parent |
| → | Expand the row; on an expanded row, move to its first child |
| Return | Show what the row names: the workspace, the tab, the pane; on a daemon row, fold it or connect |
| Shift+Return | Open what the row names in a new window |
| F2 | Rename the workspace or tab row |
| Menu, Shift+F10 | Open the row's menu, as a right click does |
| Escape | While a jump is under way, cancel it; else, while a filter is typed, clear it; otherwise leave the sidebar and give the focus back to the terminal |
| Any printable key | Start filtering |

**Type to filter.** The filter button in the sidebar's top row (a funnel; in the header bar on Linux) opens
the same field. Typing narrows the tree to rows whose title, reason or git text contains what you typed
(case-insensitive for ASCII). Every parent of a match stays and is expanded while the filter is set. The
filter is only a view: it never changes what a drag or a rename acts on.

## Mouse

- **Click** a row to show it. Click the disclosure triangle to fold or unfold it.
- **Double-click** a workspace's or a tab's name to rename it, or press **F2** on its row. Return commits,
  Escape cancels. A workspace's or a tab's name is shared: every window of the daemon and every `object.renamed` subscriber of the
  [control API](control-api.md) hears it, and `contour rename` addresses the object by it (`tab:logs`).
- **Right-click** for the row's menu. Every row's menu begins with **Open in New Window** (as Shift+Return), which
  opens what the row names in a window of its own and leaves this window where it is; a window that shows it
  already is raised instead, with the row's tab active. A row of a daemon reached by an unnamed link, or of a
  window that runs its sessions in-process, does not offer it, nor does a daemon's row while the daemon is not
  running, too old or failed. An Open in New Window under way goes on while the window switches elsewhere,
  and the other way round.
    - a daemon: Open in New Window, **New Workspace** (on the window's own daemon as the tab strip's; on
      another, made there and then jumped to), then what the row's buttons do, in their order and named in
      full as their tooltips name them (**Connect**, **Start Daemon**, **Cancel Connecting** or, while a jump
      into it is under way, **Cancel Jump**), and **Hide from Sidebar** and **Remove from Sidebar** set apart
      below them; so it stays in reach when a narrow sidebar has dropped its button;
    - a workspace: **Workspace Color**, Rename…, Mark Seen, New Tab Here, Move Up, Move Down, **Merge into
      Tab** (a workspace of one tab only), Close Workspace… (the last workspace of an in-process window cannot
      be closed);
    - a tab: the tab strip's menu (Tab Color, Rename…, and its close row reads Close Tab…), then **Split
      Vertically** and **Split Horizontally**, which split the tab's active pane, then **Move to Workspace**,
      **Merge into Tab** and **Move to New Workspace**;
    - a pane: Go to Pane, Open in New Window, Mark Seen, **Split Vertically**, **Split Horizontally**, **Move to Tab**, **Move to
      New Tab**, **Move to New Workspace**, Close Pane….

**Show, hide, remove.** Which daemons the sidebar lists is chosen in the sidebar itself. **Hide from Sidebar**
(offered on a daemon no window of this Contour is on, and not on a daemon reached by an unnamed link) takes it
off and keeps that choice for later runs; a toast says so, and its **Undo** lists it again. Hidden daemons are
gathered in the last row, "1 hidden daemon", shown while the sidebar lists everything: click it, press Return,
the Menu key or Shift+F10 on it, and its menu offers **Show ‹name›** for each. **Remove from Sidebar** takes a
daemon off for this run only, and keeps nothing. The switcher's daemon rows offer the same choice as a
check box, **Show in Sidebar** (not on the window's own daemon, nor on one any window is on, which are always
listed). Undo and Show restore what you had chosen before the Hide: a saved host whose configuration lists it,
or a daemon of this machine, merely loses its Hide; a saved host with `show_in_sidebar: false` is shown by
your choice. Every one of these is reached by keyboard through the menus; none has a default key.

  Move to Tab and Merge into Tab list every other tab, under the name of its workspace; the pane, or the
  tab's panes, join that tab as splits, as a drop on its row does. Move to New Workspace makes a workspace
  named as any new one is, which an in-process window then shows. Move to New Workspace is greyed out for a workspace's
  only tab, which already is a workspace of its own.

  Open in New Window, Workspace Color, Mark Seen, Rename…, Move Up, Move Down and the Move and Merge rows act on the row the menu
  was opened on and leave the window where it is. The rest (Go to Pane, New Tab Here, the Split rows and the
  tab strip's own commands) act on what the window shows, so the window shows that row first; a Split row
  then splits the pane the row names, or the tab's active pane.

  The three close rows (Close Workspace…, Close Tab…, Close Pane…) always ask first, in one confirmation
  that names what ends (the tabs and panes, and the running command where known). Cancel is the default
  button and Escape cancels; the answer closes the row the menu was opened on.

  Another daemon's workspace, tab and pane rows have the same menus, less what cannot cross daemons: no Move
  Up/Down, Move or Merge row, and none of the tab strip's Move Tab to New Window, Close Other Tabs and Close
  Tabs to the Right. Rename…, the colours, Mark Seen and the close rows act there in place, through that
  daemon's connection, and leave the window where it is; the confirmation ends "It runs on ‹daemon›.".
  New Tab Here and the Split rows jump to the row first and act once the jump lands, in this window or in
  the window that shows the workspace already; Go to Pane is the jump. A menu opened before its daemon
  restarted does nothing: what it names belongs to the run that ended. Such rows cannot be dragged, and
  nothing is dropped on them. A row of what was last known of a daemon that is not connected renames
  nothing.
- **Drag** to reorder or move. A row's upper and lower thirds are the gaps beside it, shown as a line where
  the drop lands; its middle is the row itself, shown highlighted. A drop the row refuses shows the refusal.
    - a workspace onto another workspace row: before it in the upper half, after it in the lower half;
    - a tab or a pane onto a tab row: in the upper or lower third, before or after that tab, as a tab (a
      pane becomes a tab of its own); in the middle, **into** that tab: the pane, or every pane of the tab,
      joins it as a split beside its active pane, on the side a drop on a tab of the tab strip takes, and
      becomes its active pane;
    - a workspace that holds one tab onto the middle of a tab row: its tab joins that tab, and the emptied
      workspace closes. A workspace of several tabs is refused there;
    - a tab or a pane onto a workspace row: to the end of it (the middle of the row), or to the start of it
      (the lower third), or to the end of the workspace above (the upper third);
    - a tab or a pane onto the tree's empty area below the last row, or onto a **New Workspace** button: a
      workspace of its own, named as any new workspace is. A workspace's only tab is refused there, as it
      already is one;
    - anything onto a pane row, or onto its own tab's middle, is refused.

  A pane that is its tab's only pane moves as that tab. Its sessions keep running wherever it goes: the
  shell, its scrollback and its status move with it, and nothing restarts.

  Drops resolve to the real positions in the daemon, so a filter or a folded row never moves where a drop
  lands. Moving the last pane out of a tab closes that tab, and moving the last tab out of a workspace closes
  that workspace, as their last pane or tab ending does. A window
  and a daemon keep their last workspace, so that one cannot be closed.
- **Drag the divider** at the sidebar's right edge to resize it. The width snaps to whole cells on the Terminal
  style, stays within the style's bounds (160 to 480 px, 18 to 60 cells) and is saved when you let go.

## A new workspace

The **＋** button, and the `NewWorkspace` action (⇧⌘N, Ctrl+Shift+Alt+N), create a
workspace and show it. The button stands where each platform keeps a sidebar's "add": in the sidebar's footer
on macOS, Windows and the Terminal style, and as a "new folder" icon in the header bar on Linux, so it is never
the title band's new-tab **+**. It is named after the **folder** the current pane is in, or, inside a git worktree,
after the **repository's root folder**, and its first shell starts in that directory. A pane with no known
directory leaves the name empty and the workspace shows as `Workspace N`. The name is written once at
creation; renaming changes it from then on. `workspace.create` of the [control API](control-api.md) does
the same, with an optional `cwd` and `name`.

## The hover card

Resting the pointer on a row opens a card with the full reason line, wrapped instead of cut off (up to 2000
characters, so a runaway one cannot flood it), and the program, the folder and the time, the folder shown to
its end. It follows the pointer onto itself, so you can read it, and closes when the pointer leaves.

## The footer

The footer counts what asks for you across **every** workspace, folded or not: `2 need you · 3 working`. It
counts panes (the leaves under each workspace), not rolled-up rows, leaves out a count that is zero, and shows
the jump key beside it.

## When the sidebar is hidden

With the sidebar away, a window on a daemon shows one **location chip** at the leading edge of its title band:
`local › billing ▾`, the daemon the window is on and the shown workspace. The default local daemon reads `local`,
a named one its name (`work`), a remote one its host, and a daemon without a name `daemon`. A window with no
daemon shows the workspace alone. The chip is marked when *another* workspace needs you, has failed, or has a
result you have not seen (done-not-seen and up). Work that is only running, or unseen output, does not mark it:
the chip is not a progress indicator. The tabs move into the title band behind the chip, following
`tab_bar_position`.

The daemon's connection shows in the chip too: a remote daemon that is connected has a green dot, a local one
shows nothing. While the link is not up, any daemon shows a dot and its state, `starfive Reconnecting…` or
`local Disconnected`. The dot keeps its shape, so colour is never the only cue, and the state is also in the chip's
accessible description.

When the band runs short, the chip gives way in this order, so the workspace name outlasts the daemon's:
the state text goes (the dot stays), then the daemon name is shortened in the middle, then the daemon shrinks to
its dot, and only then is the workspace name shortened, never below 6 characters and an ellipsis. The window cannot
be made narrower than the chip at that last step, the buttons and one tab need.

Clicking the chip, or pressing Space or Return on it, opens the switcher below it: the same popup
`ShowSwitcher` opens (`Ctrl+Shift+R`, `⇧⌘R` on macOS), with the workspaces of this window's daemon first and
typing filtering at once. The chip's tooltip, "Switch workspace or daemon", names whatever chord is bound.
The chip stays highlighted while the popup is open, and clicking it again closes it; Escape closes the popup.

With the sidebar shown, the band has no chip. The tree names the daemon in its heading row instead, and the
sidebar's title row (on Linux the header bar's part above the sidebar) carries a **switcher** button that opens
the same switcher below itself, with the same tooltip and chord. While the window's own connection is not up, the
chip comes back to the band with its dot and state, even with the sidebar shown. With several daemons listed, the
band's breadcrumb reads `daemon › workspace › tab`. A window attached to tmux has no daemon to head the tree: its
host chip, `tmuxhost ▾`, heads the sidebar as before. On a narrow sidebar the Needs-me button gives way first, then
the switcher; both are in the chrome's keyboard walk (Ctrl+F6).

- Type to filter the workspaces; Return switches this window to the selected one, Shift+Return opens it in a
  new window. The row under the pointer shows a button that does the same.
- **New Workspace** creates one on this daemon, named after what you typed, if anything.
- Below this daemon's workspaces the popup lists every other local daemon and saved host, as the
  `ShowSwitcher` popup does: the click and the chord open the same rows.
- Right-click a workspace (or press the Menu key, or Shift+F10) for the same menu its sidebar row has:
  colour, rename, mark seen, new tab, move, close.
- Escape, or a click elsewhere, closes it and changes nothing; Escape does not narrow it first.

The switcher keeps the sidebar's times running while it is open, so a working workspace's time moves on even
with the sidebar hidden.

Each daemon heads its section with a **daemon row**: its name in the platform's section-heading type (smaller
than its workspaces, semibold, in the secondary ink, as a source list heads a section), standing where its
workspaces' names stand (bold at the first column on the Terminal row, its fold mark after the name, so
it is never taken for a workspace), the computer or server icon of a local or remote daemon after the name on the
other rows, and one status at its end: the most urgent mark beneath it while it is connected, else its link's
mark and words, with **Connect**, **Start** (and **Remove**) or **Cancel** as the link offers. Start and Remove
are for daemons of this machine; a host that is not running offers neither. Connect on the window's own daemon
reconnects it, as the location chip does; while its link waits for you, it brings the prompt it waits on to the
front instead. A blank line sets each section apart. Clicking the row, Return, ← and → fold it. On a narrow
sidebar the locality icon goes first, then Remove, then the button shrinks to its icon (a one-cell mark on the
Terminal row: `→` Connect, `▶` Start, `⎋` Cancel), then the link's words move into the mark's tooltip, and last
the name elides in the middle, never to fewer than six of its letters; Connect, Start or Cancel always stays. While a jump into a section is under way its row shows `⧗`, pulsing gently unless you reduce
motion; **Cancel**, the `CancelSwitch` action (in the command palette, unbound by default) or Escape in the
sidebar ends it and leaves the window where it was (Cancel on a daemon still connecting, with no jump under
way, ends that connect). With the sidebar hidden the location chip says where the jump goes ("starfive
Connecting…"), and a click on it cancels the jump. A jump to a daemon the sidebar lists takes over the
connection that keeps its rows current rather than dialling a second one, lands on the row's tab and pane, and
the daemon you leave keeps your window's connection to stay listed live while no other window is on it; a
second click while a jump is under way goes to the second row, and a row of a daemon that has restarted since
it was listed is refused ("the daemon restarted"). Whenever a daemon is not
connected, its rows are what was last known of it: drawn in a dimmer ink, with no running mark, and counted
neither in the footer nor on the workspace chip.

In the tree and in its filter field, Escape first cancels a jump under way, then clears the filter, then gives
the keyboard back to the terminal; in the terminal it stays the shell's. Return in the filter field goes to the
first row that itself matches (a pane under a tab that does not, rather than the tab), Shift+Return opens that
row in a new window, and Shift+Return on any row of the tree does the same; with nothing typed, Return moves into
the tree.

## Tab placement

`tab_placement` chooses where the tabs of a window live:

- `sidebar`: tabs are rows of the sidebar under their workspace. While the sidebar is shown the tab strip is
  hidden and the title band shows the breadcrumb `workspace › tab`. While the sidebar is hidden, the tabs are
  in the title band as above.
- `top`: the tab strip, placed by `tab_bar_position` and shown by `tab_bar_visibility`, as before. The
  sidebar still lists the tabs.

## Configuration

```yaml
sidebar:
  visible: true     # start shown; what you last chose in a window wins over this
  width: 240        # logical pixels (cells under ui_style: terminal); omit for the style's own
  tab_color: tint   # tint | bar | name
  workspace_color: tint   # tint | group | swatch
  row_spacing: comfortable   # comfortable | compact
tab_placement: sidebar   # sidebar | top
```

`tab_color` picks how a coloured tab shows its colour on its rows, the tab's and its panes'; an uncoloured
tab looks the same in all three, and workspace rows never carry a tab's colour.

- `tint` (the default): the row is filled with the colour, faded as the tab strip fades it, and its text
  turns black or white to stay legible.
- `bar`: a narrow bar in the colour runs the row's full height at its leading edge.
- `name`: the tab's name is drawn in the colour.

`workspace_color` picks how a coloured workspace shows its colour in the sidebar, independently of
`tab_color`; an uncoloured workspace looks the same in all three, a folded workspace keeps its paint, and the
colour is shown in the sidebar only.

- `tint` (the default): the workspace's row is filled with the colour, faded as the tab strip fades a
  coloured tab, and its text turns black or white to stay legible.
- `group`: a rail in the colour runs down the sidebar's leading edge beside the workspace's row and every
  row beneath it, over a faint wash of the colour; a coloured tab's own tint is drawn over the wash.
- `swatch`: a small square in the colour stands before the workspace's name.

To colour a workspace, right-click its row and pick from **Workspace Color**: a swatch, **Custom…** for the
colour picker, or **Reset**. The `SetWorkspaceColor` action without a colour opens the picker for the shown
workspace. A workspace's colour belongs to the daemon, as its name does: every window of the daemon shows it,
and it survives a restart and a live upgrade. Against a daemon that predates workspace colours, **Workspace
Color** is greyed out, and the `SetWorkspaceColor` and `ResetWorkspaceColor` actions only say so in a notice.
A tab is coloured from **Tab Color** in its menu, as on the tab strip.

`row_spacing` picks how much room a tab's or a pane's two-line row takes; a workspace's row is one line in
both, and under `ui_style: terminal` a two-line row is two cells in both.

- `comfortable` (the default): 40 px rows on macOS, Windows and Linux. The extra room goes between the rows,
  not between a row's two lines, so each row's lines read as one.
- `compact`: 36 px rows, so more of them fit.

It is also in the Settings window, under **Appearance › Tab bar**.

`tab_color`, `workspace_color` and `row_spacing` are read when a window opens: a window that is open when the
configuration is reloaded keeps the styles it opened with, and a window opened after it uses the new ones.

See [Key mapping](configuration/key-mapping.md#workspace-sidebar) for the actions and their default chords.
The default width is 220 px on macOS, 240 px on Fluent, 260 px on ContourLinux, and 28 cells under
`ui_style: terminal`.

### Defaults

`defaults_edition: native2026`, which a newly generated configuration carries, shows the sidebar
(`sidebar.visible: true`), places the tabs in it (`tab_placement: sidebar`) and binds the chords above. A
configuration that does not set `defaults_edition` is `classic`: the sidebar is hidden, the tab strip stays on
top and the sidebar chords are not bound. Set either key explicitly to override one decision.

## The Terminal style

Under `ui_style: terminal` the sidebar is a tree of whole cells, drawn with `├ └ │`. Its width is a whole
number of cells, a one-cell `│` divides it from the terminal at one fixed column, and urgency is a glyph
only.

## Known limits

- **Ahead and behind are not shown on Windows.** They come from running `git`, and the daemon's command
  runner does not support that there. The branch still shows.
- **The sidebar has a solid background.** macOS vibrancy and Windows Mica behind it are not done yet.
- **Rows have no accessible expanded state**, because Qt's accessibility has none. The state is part of the
  row's name instead; see [Accessibility](accessibility.md#the-workspace-sidebar).
