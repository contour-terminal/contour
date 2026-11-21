# Advanced Image configuration

### Sixel scrolling mode

Enable or disable sixel scrolling (SM/RM ?80 default)

    sixel_scrolling: true

### Sixel register capacity

Configures the maximum number of color registers available
for rendering Sixel graphics.

    sixel_register_count: 4096

### Kitty file transmission

Whether the kitty graphics protocol may read image files that a local program names (`t=f`, and `t=t`
for temporary files), as Claude Code's pictures do. It applies only to sessions whose output comes from
this machine: never over SSH, in tmux panes, or for a daemon's remote clients. It is off on Windows and the BSDs for now.

    kitty_file_transmission: true

### Kitty file allow-list

Directories, besides the temporary directories, below which `t=f` may read files. A leading `~/` is your
home directory. An entry that would open your whole home directory (or `/`), a relative entry, and a
`~name` entry are ignored, each with a logged warning.

    kitty_file_allow_list: ['~/.claude/plugins/data']

See [kitty graphics](../../vt-extensions/kitty-graphics.md) for every rule a file must pass.

### Maximum image size

The maximum width and height in pixels of an image to be accepted is derived from the size of the
screen the window is on, and updates when the window moves to another screen.

Applications can query it, and lower it for themselves, via `XTSMGRAPHICS`
(`CSI ? 2 ; 4 S` reads the maximum, `CSI ? 2 ; 3 ; <width> ; <height> S` sets it).

!!! note

    `max_width` and `max_height` are **deprecated and ignored**. They are still accepted, so existing
    configurations keep loading, but no longer have any effect. Their default of `0` already meant
    "use the screen size", so only configurations that set an explicit cap change behaviour.
