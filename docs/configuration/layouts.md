# Layouts

A layout is a named set of tabs that are opened together. Each tab can change into a
directory, run a specific command instead of your shell, carry a name and a color, and
optionally be split into a tree of panes. Layouts let you reproduce a working set of
tabs — for example an editor, a chat/agent tool, and a couple of monitoring panes — with
a single action instead of opening and arranging them by hand every time.

## Configuration

Layouts are configured under the top-level `layouts` entry, a map of layout name to its
list of `tabs`. An optional top-level `default_layout` names a layout to open
automatically at startup instead of the usual single default tab.

``` yaml
layouts:
  work:
    tabs:
      - title: "editor"
        color: "#d75f00"
        directory: "~/proj/foo"
        command: "nvim"
        arguments: ["."]
        profile: "dark-big"

      - title: "claude"
        color: "green"
        directory: "~/proj/foo"
        command: "claude"

      - title: "servers"
        split:
          orientation: vertical
          panes:
            - { command: "npm run dev", directory: "~/proj/foo", ratio: 0.6 }
            - split:
                orientation: horizontal
                panes:
                  - { command: "htop" }
                  - { command: "journalctl -f" }

  dev:
    tabs:
      - title: "shell"
        directory: "~/proj/bar"

default_layout: work
```

### Tab options

Each entry under `tabs` describes either a single-pane tab (leaf fields) or a split
tab (a `split` node). A tab uses leaf fields *or* `split`, not both.

Option        | Description
--------------|--------------------------------------------------------------------
`title`       | Optional. Seeds the tab's name.
`color`       | Optional. Hex (e.g. `'#d75f00'`) or a named color; sets the tab's user color.
`directory`   | Optional working directory for the tab. Falls back to the profile's configured directory when omitted.
`command`     | Optional. Replaces the shell — the program to run in the pane. May be a full command line (see [Command lines](#command-lines) below). When omitted, the effective profile's `shell` is used.
`arguments`   | Optional list of extra arguments, appended after any arguments already given in `command`.
`profile`     | Optional per-tab profile override (see [Per-tab profile](#per-tab-profile) below).
`split`       | Optional. Turns the tab into a split node instead of a single pane; see [Split panes](#split-panes).

### Command lines

`command` may be written as a full command line — it is split into the program and its arguments the way a shell would, so both of these are equivalent:

```yaml
- command: "emacs -nw"
# is the same as
- command: "emacs"
  arguments: ["-nw"]
```

Splitting rules:

- Whitespace separates the program from its arguments (and arguments from each other).
- Quote a run to keep spaces inside one token: double quotes (`"..."`, allowing `\"` and `\\`) or single quotes (`'...'`, literal). A backslash outside quotes escapes the next character.
- If the **program's own path** contains spaces, quote it so it is not split — e.g. `command: '"/opt/My App/bin/foo" --flag'`.
- Any entries in a separate `arguments:` list are appended **after** the arguments parsed from `command`.

`SaveLayout` (below) writes the program and its arguments back out in the split form (`command:` for the program, `arguments:` for the rest), quoting a space-containing program path so it round-trips.

### Split panes

A `split` node replaces the leaf fields (`command`, `directory`, ...) with:

Option         | Description
---------------|--------------------------------------------------------------------
`orientation`  | `vertical` or `horizontal`. `vertical` arranges panes side by side; `horizontal` stacks them.
`panes`        | A list of child panes. Each child is either a leaf (with `command`, `directory`, `arguments`, `profile`, `ratio`) or another `split` node, nested recursively.
`ratio`        | Optional, set on a child pane/split. The relative size weight of that child within its parent split.

Splits can be nested arbitrarily deep, as shown in the `servers` tab of the example
above: a vertical split whose second pane is itself a horizontal split.

### Per-tab profile

A tab (or an individual pane) may set `profile` to use a different profile than the
window's own profile for that tab/pane. The override is **terminal-level only** — it
affects things like the shell, fonts, colors, and scrollback of that pane. Window-level
settings (window size, decorations, and similar) always come from the profile that
launched the window itself.

## Opening a layout

There are three ways to open a layout:

### At startup

Set `default_layout` to a layout name; it opens automatically when Contour starts,
replacing the usual single default tab.

``` yaml
default_layout: work
```

### From the command line

Pass `--layout NAME` to open a layout in the newly launched window:

``` bash
contour terminal --layout work
```

### From a keybinding

Bind the `LaunchLayout` action to a key, passing the layout's `name`:

``` yaml
input_mapping:
  - { mods: [Control, Shift], key: L, action: LaunchLayout, name: work }
```

Unlike the startup and CLI cases, `LaunchLayout` **appends** the layout's tabs to the
current window rather than opening a new one.

## Saving the current window as a layout

The `SaveLayout` action captures the active window's current tabs and panes — their
directories, the command each pane was started with, titles, colors, and split
structure — and writes them out as a named layout.

``` yaml
input_mapping:
  - { mods: [Control, Shift], key: S, action: SaveLayout, name: work }
```

`SaveLayout` never modifies your hand-written configuration file. Instead it writes a
machine-managed `layouts.yml` file next to your main configuration file, in the same
directory. Layouts from `layouts.yml` are merged with any `layouts` defined inline in
your main configuration; if a name exists in both, the `layouts.yml` version wins, since
it reflects the most recently saved state.
