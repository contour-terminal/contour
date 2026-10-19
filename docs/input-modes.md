# Input Modes

Normally, a terminal emulator only knows about one input mode, so
there is no need of distinction.

Inspired by Vi/Vim, the Termite terminal emulator started to introduce
so called Vim-modes, where the user can use the keyboard only to
screen text selection, amongst other things. So users can press
Ctrl+Shift+Space to enter the vim mode. You can then move the cursor
using vim motion keys and then start selecting.

### Contour introduces multiple Vim-like input modes:

- **insert mode**: This is the default input mode. Everything is forwarded to the application.
- **normal mode**: Use motion keys to move the cursor and operators to act
- **visual mode**: Linear selection, use motion keys to alter the selection.
- **visual line mode**: Line based selection, use motion keys to alter the selection.
- **visual block mode**: Block based selection, use motion keys to alter the selection.
- **[hint mode](demo/hint-mode.md)**: Overlay alphabetic labels on detected patterns (URLs, file paths, etc.) and type a label to act on it.

### Supported operators (Normal Mode)

- Normal: `[count] p` (paste primary clipboard `count` times)
- Normal: `yy` (yank current line to primary clipboard)
- Normal: `y {motion}` (yank given `motion` to primary clipboard)
- Normal: `y {TextObject}` (yank given `textObject` to primary clipboard, such as `yiw`, `yaw`, `yip`, `yap`, ...)
- Visual: `y` (yank current selection into primary clipboard)
- `v` enables/disables visual mode
- `V` enables/disables visual line mode
- `i` activates insert mode
- `Ctrl+v` enables/disables visual block mode
- Normal: `o {motion}` (opens resource by given `motion`, this can be a URL or local file)
- Normal: `o {TextObject}` (opens resource by given `textObject`, such as `oiw`, `oaw`, `oiW`, ...)
- Visual: `o` (open current selection into primary clipboard)
- Normal: `gh` (enter [hint mode](demo/hint-mode.md) — copy selected match to clipboard)
- Normal: `gH` (enter [hint mode](demo/hint-mode.md) — open selected match)
- Normal: `gx` (opens the link under the cursor: a URL, a path, or a custom pattern)
- Normal: `mm` (set or clear a [user mark](demo/line-marks.md#bookmarking-a-line-in-vi-normal-mode) on the cursor line)
- Normal: `za` / `zo` / `zc` (toggle / open / close the [command block](demo/command-blocks.md#folding) under the cursor)
- Normal: `zM` / `zR` (close / open every command block)

### Supported motions

Moving the cursor outside of the current view using a motion, will cause
the terminal to scroll the view to make that target line visible.

- `[count] h`
- `[count] j`
- `[count] k`
- `[count] l`
- `[count] w`
- `[count] b`
- `[count] e`
- `[count] |`
- `0`
- `$`
- `gg`
- `G`
- `{` & `}`
- `[m` & `]m` (previous / next mark: a prompt or a user mark)
- `[count] ]x` (next link)
- `[count] [x` (previous link)

### Supported text objects

- `i<`, `a<` - angle brackets enclosed text
- `i{`, `a{` - curly brackets enclosed text
- `i"`, `a"` - double quotes enclosed text
- `ip`, `ap` - backtick enclosed string
- `i(`, `a(` - round brackets enclosed string
- `i'`, `a'` - single quoted string
- `` i` ``, `` a` `` - backtick enclosed string
- `i[`, `a[` - square bracket enclosed string
- `iw`, `aw` - regular word
- `iW`, `aW` - space delimited word
- `im`, `am` - the lines between two marks, without / with the marked lines

### Opening local files and URLs

Contour finds URLs, file paths and patterns of your own in the output and underlines them
(see [Links in Terminal Output](configuration/links.md)). In normal mode, `]x` and `[x` move the
cursor from one link to the next, scanning beyond the screen when needed, and `gx` opens the link
under the cursor as Ctrl+click would. A path counts once Contour has found that it exists.

You can also open what is not detected as a link by moving the cursor onto a resource (URL or local
file) and then pressing `oiW` (open in (space separated) word) or use any of the other supported text
objects to denote what to open.

You may as well type `oo` to open the URL under the current line. This works if no other information
is on this current line, while spaces are trimmed left and right of the given URL in this line.
