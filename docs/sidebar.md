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

A workspace row is one line; a tab or pane row has a status glyph, a title, a time on the right and, when its
program has something to say, a second line with the reason.

| Row | Title | Right-hand side | Second line |
| --- | --- | --- | --- |
| Workspace | Its label, or the folder or repository it was started in | The git branch with `↑` commits ahead and `↓` behind its upstream, such as `feat/billing ↑2 ↓1`, then the status glyph of its most urgent pane | None |
| Tab | The tab's title | Time in the current state | What the program in it says it is doing |
| Pane | Shown only for a tab with more than one pane | The same | The same |

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
- **Failed.** A failed status is drawn as the red ❌ on the native rows, in the sidebar, the workspace chip and
  the tab strip's badge; the Terminal style keeps a single-cell red ✕, because ❌ is two cells wide.

## Urgency

One order ranks every status, in the sidebar, the tab strip, the taskbar and the workspace chip, most urgent
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
| `CycleWorkspace`: show the next / previous workspace | ⌃⌘↓ / ⌃⌘↑ | Ctrl+Shift+Alt+PageDown / PageUp |
| `FilterWorkspaces`: open the filter, showing the sidebar first when hidden | none | none |

These are bound in the `native2026` defaults only, except `CycleWorkspace`, which every defaults edition binds,
one level up from the tab chords (Ctrl+PageUp/PageDown). `FilterWorkspaces` and `NewWorkspace` have no default chord;
bind one in `input_mapping`. Each control's tooltip names the chord its action is bound to at the time, so a
binding you add shows there at once.

By pointer, the **sidebar toggle** at the start of the title band (beside the traffic lights on macOS, at the
header bar's start on Linux, in the sidebar's own top row while a macOS sidebar is shown) hides and shows it,
from the same spot both ways. The title bar's menu has **Show Sidebar** / **Hide Sidebar**, and so has the View
menu on macOS. When no title band is on screen (full screen, or `tab_bar: never` with the title bar hidden) and
the sidebar is hidden, the terminal's own right-click menu offers **Show Sidebar**, so the sidebar can always
be brought back the way it went. The width you drag it to and whether you left it shown are
remembered per user and win over the configured start state.

## Keyboard

With the focus in the sidebar (**⌘⇧E** or **Ctrl+Shift+D**):

| Key | Does |
| --- | --- |
| ↑ / ↓ | Move to the previous or next row |
| Home / End | Move to the first or last row |
| ← | Collapse the row; on a collapsed or leaf row, move to its parent |
| → | Expand the row; on an expanded row, move to its first child |
| Return | Show what the row names: the workspace, the tab, the pane |
| F2 | Rename the workspace or tab row |
| Menu, Shift+F10 | Open the row's menu, as a right click does |
| Escape | While a filter is typed, clear it; otherwise leave the sidebar and give the focus back to the terminal |
| Any printable key | Start filtering |

**Type to filter.** The filter button in the sidebar's top row (a funnel; in the header bar on Linux) opens
the same field. Typing narrows the tree to rows whose title, reason or git text contains what you typed
(case-insensitive for ASCII). Every parent of a match stays and is expanded while the filter is set. The
filter is only a view: it never changes what a drag or a rename acts on.

## Mouse

- **Click** a row to show it. Click the disclosure triangle to fold or unfold it.
- **Double-click** a workspace's or a tab's name to rename it. Return commits, Escape cancels. A workspace's
  label is shared: every window of the daemon and every `workspace.labelChanged` subscriber of the
  [control API](control-api.md) hears it.
- **Right-click** for the row's menu:
    - a workspace: Rename…, Mark Seen, New Tab Here, Move Up, Move Down, **Merge into Tab** (a workspace of
      one tab only), Close Workspace… (the last workspace of an in-process window cannot be closed);
    - a tab: the tab strip's menu (its close row reads Close Tab…), plus **Move to Workspace**, **Merge into
      Tab** and **Move to New Workspace**;
    - a pane: Go to Pane, Mark Seen, **Move to Tab**, **Move to New Tab**, **Move to New Workspace**, Close
      Pane….

  Move to Tab and Merge into Tab list every other tab, under the name of its workspace; the pane, or the
  tab's panes, join that tab as splits, as a drop on its row does. Move to New Workspace makes a workspace
  named as any new one is, which an in-process window then shows. Move to New Workspace is greyed out for a workspace's
  only tab, which already is a workspace of its own.

  Mark Seen, Rename…, Move Up, Move Down and the Move and Merge rows act on the row the menu was opened on and
  leave the window where it is. The rest (Go to Pane, New Tab Here and the tab strip's own commands) act on
  what the window shows, so the window shows that row first.

  The three close rows (Close Workspace…, Close Tab…, Close Pane…) always ask first, in one confirmation
  that names what ends (the tabs and panes, and the running command where known). Cancel is the default
  button and Escape cancels; the answer closes the row the menu was opened on.
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

The **＋** button, and the `NewWorkspace` action (not bound by default), create a
workspace and show it. The button stands where each platform keeps a sidebar's "add": in the sidebar's footer
on macOS, Windows and the Terminal style, and as a "new folder" icon in the header bar on Linux, so it is never
the title band's new-tab **+**. It is named after the **folder** the current pane is in, or, inside a git worktree,
after the **repository's root folder**, and its first shell starts in that directory. A pane with no known
directory leaves the label empty and the workspace shows as `Workspace N`. The name is written once at
creation; renaming changes it from then on. `workspace.create` of the [control API](control-api.md) does
the same, with an optional `cwd` and `label`.

## The hover card

Resting the pointer on a row opens a card with the full reason line, wrapped instead of cut off (up to 2000
characters, so a runaway one cannot flood it), and the program, the folder and the time. It follows the pointer onto itself, so you can read it, and closes when the
pointer leaves.

## The footer

The footer counts what asks for you across **every** workspace, folded or not: `2 need you · 3 working`. It
counts panes (the leaves under each workspace), not rolled-up rows, leaves out a count that is zero, and shows
the jump key beside it.

## When the sidebar is hidden

With the sidebar away, the title band shows a **workspace chip**: the shown workspace's name. The chip is
marked when *another* workspace needs you, has failed, or has a result you have not seen (done-not-seen and
up). Work that is only running, or unseen output, does not mark it: the chip is not a progress indicator.
The tabs move into the title band behind the chip, following `tab_bar_position`.

Clicking the chip drops this window's workspaces down below it, the ones of the daemon the window is on and
nothing else; pick one to show it here, or press Escape or click elsewhere to close it. The whole daemon and
workspace switcher stays on its `ShowSwitcher` key.

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
tab_placement: sidebar   # sidebar | top
```

See [Key mapping](configuration/key-mapping.md#workspace-sidebar) for the actions and their default chords.
The default width is 220 px on macOS, 240 px on Fluent and ContourLinux, and 28 cells under
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
