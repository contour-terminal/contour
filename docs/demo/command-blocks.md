# Command blocks

With [shell integration](../vt-extensions/osc-133-shell-integration.md) installed, every command you
run becomes a **command block**: the prompt you typed it at and the output it printed. For each block
Contour remembers the command line, how it ended, when it started, how long it took and the directory
it ran in — for the last 1000 commands by default (`command_blocks.max_records`), even after their
output has scrolled out of the history.

Most of this page is built on those records. Without shell integration — and without
[OSC 3008](../vt-extensions/osc-3008-context.md), which systemd 258 and later send on their own — there
are no command blocks: no block marks in the gutter or on the scrollbar, no sticky header and no finish
notifications. Line numbers, timestamps and user marks work either way.

## The gutter

The strip left of the grid holds up to three columns, each switched on by configuration. Its width
depends only on the configuration, never on what is on screen — a gutter that grew would resize the
page mid-command. On the alternate screen of a full-screen program such as vim, less or htop it keeps
its width and stays empty.

| Column | Shows | Configured by | Default |
|--------|-------|---------------|---------|
| Timestamps | when each line was first reached | `gutter.timestamps`, `gutter.timestamp_format` | off |
| Line numbers | absolute, relative or hybrid numbers | `gutter.line_numbers`, `gutter.line_number_width` | off |
| Block column | fold controls, exit status, user marks | `folding.show_markers`, `gutter.exit_status`, `gutter.user_marks` | on |

```yaml
gutter:
  exit_status: true
  user_marks: true
  line_numbers: hybrid
  line_number_width: 6
  timestamps: true
  timestamp_format: "%H:%M:%S"
```

### The block column

One cell per line, beside the grid, showing the first of these that applies:

| Mark | Where | Meaning |
|------|-------|---------|
| boxed minus / boxed plus | a prompt line | the block's output is shown / collapsed; click to toggle |
| ◆ | any other line | a user mark (`mm` in Vi normal mode) |
| `│`, `┕` | the output | the extent of the block |
| ▸ | a prompt line | the command is still running |
| ● | a prompt line | the command finished without output to fold — or, with `folding.show_markers: false`, finished at all |

A block's marks take the color of how its command ended, from the color scheme's
[`block_status`](../configuration/colors.md#block_status) colors: neutral for success, red for a
failure, yellow while it runs. With `gutter.exit_status: false` the column keeps the plain fold-marker
color and draws neither ▸ nor ●; with `gutter.user_marks: false` it draws no ◆. The column is there while
any of the three is on.

Hover the block column to see the block's exit code (and the signal that ended it, named by
[OSC 3008](../vt-extensions/osc-3008-context.md) or by a 128 + n exit status), its duration, start time,
directory and command line.

### Line numbers

`gutter.line_numbers` works like vim's `number` and `relativenumber`:

| Value | vim | Cursor line | Other lines |
|-------|-----|-------------|-------------|
| `off` | `nonu nornu` | — | — |
| `absolute` | `nu` | its number | their numbers |
| `relative` | `rnu` | `0` | distance from the cursor line |
| `hybrid` | `nu rnu` | its number | distance from the cursor line |

- An absolute number counts rows from the first row of the session, including rows that have since left
  the history, so a row keeps its number while the history scrolls away. Resizing the window re-wraps the
  lines and renumbers the rows below the top.
- A relative distance counts the rows you see: a collapsed block counts as one row, exactly as `j` and `k`
  do, so `12k` lands on the row labeled 12.
- The cursor is the Vi-mode cursor while Vi mode is on, and the terminal's cursor otherwise.
- `gutter.line_number_width` (3 to 10, default 6) is the column count; a longer number shows its last
  digits after `…`.

### Timestamps

`gutter.timestamps: true` shows beside each line the local time output first reached it.
`gutter.timestamp_format` is a [`std::format` chrono format](https://en.cppreference.com/w/cpp/chrono/system_clock/formatter)
such as `%H:%M:%S` (the default) or `%H:%M`. It must begin with a `%` conversion, and every character it
spells must take exactly one column; a format that cannot be used is reported when the configuration loads,
and the default is kept. A line shows its time only where it differs from the line above, so a burst of
output does not repeat the same second all the way down. Recording the time costs no memory per line.

### Folding

A finished command's output can be collapsed to its prompt line (see
[`folding`](../configuration/index.md#folding)): click its fold control in the block column, or use the
`ToggleFold`, `ToggleLastFold`, `CollapseLastFold`, `CollapseAllFolds` and `ExpandAllFolds` actions. A
collapsed block says how much it hides — `⋯ 1,234 lines` — after its prompt; that label is drawn over the
page and is never selected or copied. In Vi normal mode, vim's fold keys work:

| Key | Action |
|-----|--------|
| `za` | toggle the block under the cursor |
| `zo` | open it |
| `zc` | close it |
| `zM` | close every block |
| `zR` | open every block |

Unlike vim's, these know one fold level only, take no count, and work in normal mode alone; `zn`, `zN` and
`zi` do not exist. With `folding.enabled: false`, `za`, `zc` and `zM` do nothing, while `zo` and `zR` still
open what is collapsed.

## Scrollbar marks

The scrollbar marks where commands are, so a failure far up the history is one glance — and one
click — away. Clicking a mark scrolls its line to the top, opening its block if that was collapsed.
Marks that fall on the same pixel merge, the most important winning: a failure, then a running command,
then a user mark, then any other command. A command's mark takes its
[`block_status`](../configuration/colors.md#block_status) color.

```yaml
profiles:
  main:
    scrollbar:
      marks: [failures, commands, user_marks]   # [] turns the marks off
```

The scrollbar is shown on the right by default, so the marks are there from the start. It floats over the
edge of the terminal without taking a column away, and appears only once there is history to scroll
through.

That default applies to new configurations. Contour writes out its full default configuration on first
run, so a `contour.yml` written by an earlier version still says `position: Hidden` and shows neither the
scrollbar nor its marks. Set `position: Right` to see them, or pick *right* for *Scrollbar position* on
the settings page:

```yaml
profiles:
  main:
    scrollbar:
      position: Right      # Left | Right (the default) | Hidden
```

The marks hide whenever the scrollbar does: on the alternate screen (`hide_in_alt_screen`, on by
default), and with `position: Hidden`, or *hidden* on the settings page.

## Sticky command header

Scrolled back into a long output, the command that produced it stays pinned above it with its exit
status and duration — or `running 12s` while it runs. Click the header to jump to the command.

```yaml
profiles:
  main:
    sticky_header:
      mode: scrolled       # never | scrolled | always
      show_evicted: true
```

- `scrolled` (the default) shows the header only while you are scrolled back into the history;
  `always` also shows it while following live output, so a running `make` stays labeled above its
  own output; `never` turns it off. It never shows on the alternate screen.
- The header needs prompt marks (`OSC 133 ; A`): a shell that marks only where its commands run gets none.
- When a command's own line has already been evicted from the history, the header names the command
  instead — `⋯ make -j8 — earlier output evicted` — so its remaining output still says where it came
  from. It does so wherever the header would show, and at the very top of the history even with
  `mode: never`; `show_evicted: false` turns it off.

The header is drawn over the page: selection, copying and screen readers see only the page.
Its colors: [`sticky_header`](../configuration/colors.md#sticky_header).

## Acting on a block

| To | Use |
|----|-----|
| select a command's output | <kbd>Ctrl</kbd> + triple-click on it, or `SelectCommandBlock` |
| copy part of a command block — its output, its input line, or both — as plain text | `CopyCommandBlock` |
| copy a command line, safe to paste | `CopyCommandLine` |
| open the last command's output in a pager | <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>G</kbd>, or `OpenCommandOutput` |
| select, copy or page the block under the pointer | the context menu |
| clear everything above the current prompt | `ClearToPrompt` |
| copy the last command's output when nothing is selected | `CopySelection` with `fallback: last_command_output` |

```yaml
input_mapping:
  # Ctrl+triple-click: select the clicked command's output (built in; a row like this replaces it)
  - { mods: [Control], mouse: Left, clicks: 3, action: SelectCommandBlock, part: output, target: pointer }
  # Ctrl+Shift+G: the last command's output in the pager (built in; command_blocks.pager)
  - { mods: [Control, Shift], key: G, action: OpenCommandOutput, target: last }
  # Copy the last command's output, or its command line, without selecting anything
  - { mods: [Control, Alt], key: C, action: CopyCommandBlock, target: last, part: output }
  - { mods: [Control, Alt], key: P, action: CopyCommandLine, target: last }
  # Copy the selection -- or, with nothing selected, the last command's output
  - { mods: [Control, Shift], key: C, action: CopySelection, fallback: last_command_output }
```

- `SelectCommandBlock` takes `part` — `output`, `input` (the prompt with the command typed at it) or `all` —
  and `target`: `pointer` (the block under the mouse), `cursor` (the block holding the cursor, the Vi-mode
  cursor in Vi mode) or `last` (the newest finished one).
- `CopyCommandBlock` takes the same `part` and `target` as `SelectCommandBlock` and copies that part
  into the clipboard as plain text. `CopyCommandLine` takes a `target` and copies that block's command
  line with the control characters that would be unsafe to paste removed, so an embedded end-of-paste
  sequence cannot make a shell run the rest.
- `OpenCommandOutput` takes the same `target`, plus a `program` that defaults to `command_blocks.pager`
  (`less -R`, or `more` on Windows). Its `placement` is `split` (a new pane below, the default), `tab`, or
  `detached`, which pipes the output into the program as plain text and opens no pane; for a pane, the
  output goes to a private file that is deleted when the pane closes. Its `format` is `sgr`, which keeps the
  colors, or `plain`, which drops them; without one, the configured pager gets
  `command_blocks.pager_format` (`sgr` by default) and a `program` the binding names gets `sgr`.
- `CopySelection`'s fallback is off unless you ask for it, so copying with nothing selected never
  replaces your clipboard with a build log by accident.
- The context menu's *Select Output*, *Copy Output*, *Copy Command* and *Open Output in Pager* act on the
  block under the pointer.
- Relative file paths in a command's output — <kbd>Ctrl</kbd>+clicked, or picked in
  [hint mode](hint-mode.md#path-resolution) — resolve against the directory that command ran in, not the
  shell's current one. Paths from a command that ran in a container or over ssh are not opened locally.

!!! note "New bindings and an existing `input_mapping`"

    Defining `input_mapping` replaces the default bindings (see [Key mapping](../configuration/key-mapping.md)).
    That costs you nothing here: <kbd>Ctrl</kbd>+triple-click and <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>G</kbd>
    are built-in fallbacks, not defaults, so they work whatever your `input_mapping` says — unless it
    binds the same chord itself, in which case your binding wins. `CopyCommandBlock`, `CopyCommandLine`
    and `ClearToPrompt` have no chord at all: add the bindings you want to your `input_mapping`.

## Finish notifications

When a command that took a while finishes somewhere you are not looking, Contour posts a desktop
notification — `✓ make finished`, `✗ make failed (exit 2)` or `✗ make killed (SIGSEGV)` — with how long it
took, where it ran and which tab it is in. Clicking it brings that pane forward where the desktop reports
clicks, and a newer notification from the same pane replaces the older one.

```yaml
profiles:
  main:
    notify_on_command_finish:
      when: unfocused      # never | unfocused | hidden | always
      min_duration: 10     # seconds; quicker commands never notify
      outcome: any         # any | failure
      action: notify       # notify | bell | notify_bell
      clear_on: focus      # focus | next | never
```

| Key | Meaning |
|-----|---------|
| `when` | `unfocused`: unless the pane is the focused one of the active window (the default); `hidden`: only while the pane is off screen (another tab, a zoomed sibling, a minimized window); `always`; `never` |
| `min_duration` | commands quicker than this many seconds never notify (default 10) |
| `outcome` | notify for `any` command, or only for a `failure` (a non-zero exit or a signal) |
| `action` | a desktop notification (`notify`), the profile's bell (`bell`), or both (`notify_bell`) |
| `clear_on` | withdraw the notification when you focus the pane (`focus`), when the next command there starts (`next`), or `never`. Only a desktop notification can be withdrawn (Linux); a tray message elsewhere stays until it times out |

It is on by default because the case it serves — a build in a background tab — is exactly when nobody
thinks to turn it on; the 10-second floor keeps ordinary commands quiet.

## User marks

`mm` in Vi normal mode marks the cursor line, and clears the mark again. A user mark is drawn as ◆ in
the gutter and as a tick on the scrollbar, and `[m` / `]m` jump between marks of both kinds. Unlike a
prompt, it never starts a command block: folding and block-wise history eviction ignore it. See
[Line marks](line-marks.md#bookmarking-a-line-in-vi-normal-mode).

## Daemon sessions

All of this works in a [`contour client`](../persistent-sessions.md) session attached to a daemon: the
daemon sends the block records along with the screen. Reattaching does not replay notifications for
commands that finished while you were away. `OpenCommandOutput` opens no pane there — the pane would run
on the daemon's side, which never sees the file — so bind it with `placement: detached` instead.

## Settings page

Every setting above but the key bindings can also be changed on the settings page:

- *Global settings* has the rows named *Gutter: …*, *Command blocks: …* and *Folding: …*
  ([configuration](../configuration/index.md)). Each saves the one key it edits to `settings.yml`, and the
  rest of its section keeps what `contour.yml` says. *Command blocks: pager format* picks `sgr` or `plain`.
  *Command blocks: pager* cannot be left blank; *Reset* goes back to the pager `contour.yml` names.
- A profile's page has a *Command blocks* group ([profiles](../configuration/profiles.md)): the three
  *Scrollbar marks: …* rows, *Sticky command header*, *Name evicted commands*, *Notify on finish* and the
  *Notify: …* rows. *Scrollbar position* is under *History & scrollbar*. A profile defined in `contour.yml`
  is read-only there; *Save As* makes an editable copy.
- A color scheme's page has the swatches *Block status: success*, *failure* and *running*, *Gutter text*,
  and *Sticky header: background* and *separator* ([colors](../configuration/colors.md)). A swatch the
  scheme leaves unset says that it follows the scheme's other colors, and shows the color they give it.
  Typing a new color pins the swatch, and so does <kbd>Enter</kbd> in its field, which keeps the color it
  shows now; tabbing through the field pins nothing. Each of these swatches that the scheme sets has a ↺
  button, which makes it follow the other colors again.
- *Keybindings* lists the bindings without editing them: the `input_mapping` of `contour.yml`, then the
  built-in bindings none of its rows takes, such as <kbd>Ctrl</kbd>+triple-click and
  <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>G</kbd>, marked *built-in*. A binding that runs several actions
  names them all.

When the page refuses a value, a message above the rows says why, and screen readers announce it. The
message goes away with the next change that goes through, or when you open another page.

## Configuration reference

- Global: [`gutter`](../configuration/index.md#gutter), [`command_blocks`](../configuration/index.md#command_blocks), [`folding`](../configuration/index.md#folding)
- Profile: [`scrollbar`](../configuration/profiles.md#scrollbar), [`sticky_header`](../configuration/profiles.md#sticky_header), [`notify_on_command_finish`](../configuration/profiles.md#notify_on_command_finish)
- Color scheme: [`block_status`](../configuration/colors.md#block_status), [`gutter_text`](../configuration/colors.md#gutter_text), [`sticky_header`](../configuration/colors.md#sticky_header)
- Actions and bindings: [Key mapping](../configuration/key-mapping.md)
