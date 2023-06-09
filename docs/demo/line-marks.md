# Line Marks

Ever wanted to jump quickly to the top of the previous prompt?
With a little bit of shell integration, you can make the shell tell
the terminal which lines in your screen and scrollback buffer to remember.

## Setting a line mark

This is what a shell integration would do, but you can simply
mark lines yourself by trivially writing to `stdout`, as follows:

```sh
printf "\033[>M"
```

This will tell the terminal to remember the line as a jump-target

## Jump via shortcut

Ensure you have a similar configuration set as follows to relatively jump
up or down of your marked lines.

```yaml
input_mapping:
    - { mods: [Control, Alt], key: K, action: ScrollMarkUp,   mode: "~Alt" }
    - { mods: [Control, Alt], key: J, action: ScrollMarkDown, mode: "~Alt" }
```

## Jump extension to Vi-like Normal Mode

Use `[m` and `]m` to jump the the next line mark up and down when being in
normal input mode.

Please see [Input Modes](../input-modes.md) for more information.

## Line marks as text objects

In Vi-like normal mode, you can span a text object in between two line marks,
as follows:

- `vim` - visual select within two line marks (excluding marked lines)
- `vam` - visual select around two line marks (including marked line)
- `yim` - yank within two line marks (excluding marked lines)
- `yam` - yank around two line marks (including marked line)

Please see [Input Modes](../input-modes.md) for more information.
