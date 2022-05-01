# Contour Input Modes

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

### Supported text objects

- `i<`, `a<` - angle brackets enclosed text
- `i{`, `a{` - curly brackets enclosed text
- `i"`, `a"` - double quotes enclosed text
- `ip`, `ap` - backtick enclosed string
- `i(`, `a(` - round brackets enclosed string
- `i'`, `a'` - single quoted string
- `i\``, `a\`` - backtick enclosed string
- `i[`, `a[` - square bracket encoded string
- `iw`, `aw` - (space delimited) word
