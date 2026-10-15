# Line Marks

Ever wanted to jump quickly to the top of the previous prompt?
With a little bit of shell integration, you can make the shell tell
the terminal which lines in your screen and scrollback buffer to remember.

## Setting a line mark

This is what a shell integration would do, but you can simply
mark lines yourself by trivially writing to `stdout`, as follows:

```sh
printf "\033]133;A\033\\"
```

This will tell the terminal to remember the line as a jump-target. Being a prompt mark, it also
starts a new command block; to mark a line without that, see
[Bookmarking a line in Vi normal mode](#bookmarking-a-line-in-vi-normal-mode).

`OSC 133 ; A` is the shell-integration sequence for "a new prompt starts here". Contour also
accepted a dedicated `SETMARK` sequence (`CSI > M`) for this until 0.7.1, when it was removed in
favour of this one.

## Jump via shortcut

Ensure you have a similar configuration set as follows to relatively jump
up or down of your marked lines.

```yaml
input_mapping:
    - { mods: [Control, Alt], key: K, action: ScrollMarkUp,   mode: "~Alt" }
    - { mods: [Control, Alt], key: J, action: ScrollMarkDown, mode: "~Alt" }
```

## Jump extension to Vi-like Normal Mode

Use `[m` and `]m` to jump to the next line mark up and down when being in
normal input mode.

Please see [Input Modes](../input-modes.md) for more information.

## Bookmarking a line in Vi normal mode

In Vi-like normal mode, `mm` toggles a *user mark* on the line the cursor is on. A user mark is a
bookmark, not a prompt: `[m`/`]m`, the `im`/`am` text objects and `ScrollMarkUp`/`ScrollMarkDown`
visit user marks and prompt marks alike, but a user mark never starts a command block, so it neither
splits a fold nor decides where old output is dropped from the scrollback.

A user mark is drawn as ◆ in the gutter and as a tick on the scrollbar; see
[Command blocks](command-blocks.md#user-marks).

## Line marks as text objects

In Vi-like normal mode, you can span a text object in between two line marks,
as follows:

- `vim` - visual select within two line marks (excluding marked lines)
- `vam` - visual select around two line marks (including marked line)
- `yim` - yank within two line marks (excluding marked lines)
- `yam` - yank around two line marks (including marked line)

Please see [Input Modes](../input-modes.md) for more information.
