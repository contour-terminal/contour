# Kitty graphics: Unicode placeholders and file media

Contour speaks the [kitty graphics protocol](https://sw.kovidgoyal.net/kitty/graphics-protocol/):
images transmitted with `APC G … ST`, placed at the cursor, animated, and deleted again. This page
describes the two parts of it that let a program draw a picture *as text*: **Unicode placeholders**,
and the **file media** such programs send their pictures with. Together they are what Claude Code
needs to show a Mermaid diagram as a picture rather than as source.

## Unicode placeholders

A normal kitty placement draws an image at the cursor, and the image then lives beside the text
rather than in it. A placeholder placement turns that around: the program prints ordinary
characters, and the terminal draws a piece of the image in each of their cells. Because the
picture is text, it scrolls, wraps, clears and survives an application that only knows how to print
— which is why tmux-aware programs and full-screen UIs prefer it.

### A virtual placement

```
ESC _G a=T,U=1,i=<id>,c=<columns>,r=<rows>,f=100;<payload> ESC \
```

`U=1` on `a=T` (transmit and place) or `a=p` (place) creates a **virtual placement**: it writes no
cell and leaves the cursor where it was. It names the image (`i=`), an optional placement id (`p=`),
and the span in cells (`c=`, `r=`; without them, the image's size in cells, rounded up). An image may
have several virtual placements.

As in kitty, a virtual placement always fits the **whole image** into its span, keeping its aspect
ratio and centring it; the source rectangle keys `x=`, `y=`, `w=` and `h=` are ignored. Where the
image does not fill the span, the cell's own background shows.

### The placeholder cells

Each cell of the picture is the character `U+10EEEE`, followed by combining marks from kitty's
[row/column diacritic table](https://sw.kovidgoyal.net/kitty/_downloads/f0a0de9ec8d9ff4456206db8e0814937/rowcolumn-diacritics.txt):

| What | Says |
| ---- | ---- |
| foreground colour | the image id: an indexed colour `38;5;<n>` is id `n`, a true colour `38;2;r;g;b` is `r<<16 \| g<<8 \| b`, the default colour is id 0 |
| first diacritic | the row of the image this cell shows |
| second diacritic | the column |
| third diacritic | the image id's most significant byte (for ids above 2²⁴) |
| underline colour | the placement id; absent or 0 means the image's first virtual placement |

Diacritics may be left out, and Contour infers them as kitty does: a cell without a column continues
its left neighbour's row and column, and one without a row takes its left neighbour's row. A
placeholder cell is always one column wide.

```sh
# A 2x4 image 42 drawn at the cursor: one virtual placement, then two rows of placeholder cells.
printf '\033_Ga=T,U=1,i=42,c=4,r=2,f=100,q=2;%s\033\\' "$(base64 -w0 picture.png)"
printf '\033[38;5;42m\U10EEEE̅̅\U10EEEE̅̍\U10EEEE̅̎\U10EEEE̅̐\033[m\n'
printf '\033[38;5;42m\U10EEEE̍̅\U10EEEE̍̍\U10EEEE̍̎\U10EEEE̍̐\033[m\n'
```

The cells are resolved at the end of each read from the program, a whole line at a time, so a cell
whose diacritics arrive in a later read still shows its piece; a cell printed before its image or
placement exists is resolved once they arrive. A cell that names no image shows nothing. The
`U+10EEEE` characters are never drawn as glyphs, while the grid keeps them as received: copying,
reflow and `DECRQCRA` see what the program wrote. Writing over a placeholder cell removes its piece of
the picture.

### Deleting

| Delete | Reaches virtual placements |
| ------ | -------------------------- |
| `d=i` / `d=I` with `i=` (and `p=`) | yes: the image's placements of both kinds; upper case frees the image once none remains |
| `d=n` / `d=N` with `I=` (image number) | yes, as `d=i`/`d=I` for the newest image of that number |
| `d=r` / `d=R` with `x=`, `y=` (an id range) | yes, as `d=i`/`d=I` for every id in the range |
| `d=a` / `d=A` | no: placeholder cells stay; `d=A` frees no image a virtual placement still holds |
| positional targets (`c`, `p`, `q`, `x`, `y`, `z`) | not supported (`ENOTSUP`) |

Image numbers (`I=`) are supported: transmitting with `I=` and no `i=` picks a free id and replies
`i=<id>,I=<number>`.

### Querying support

kitty's feature query tells a program whether placeholders are understood:

```
ESC _G a=q,i=31,e=T,U=1 ESC \      →      ESC _G i=31,e=T,U=1;OK ESC \
```

The reply echoes the keys Contour supports for the queried action (`e=`), so one that is missing is
one Contour does not support. A query without `i=` gets no reply, as in kitty.

## File media

Besides direct transmission (`t=d`, the payload in the sequence itself), a program may name a file
for the terminal to read:

| Medium | | |
| ------ | - | - |
| `t=d` | direct | always |
| `t=f` | a file | read under the policy below |
| `t=t` | a temporary file | as `t=f`, deleted after it is read; its name must contain `tty-graphics-protocol` |
| `t=s` | POSIX shared memory | **not supported**: it would let any program writing to the terminal unlink any shared memory object the user owns |

A file is read, and a `t=t` file deleted, only when every one of these holds:

- it lies below one of the **roots**: the temp directories (`$TMPDIR` and `/tmp` on Linux; `$TMPDIR`,
  `/private/tmp` and the per-user temp directory on macOS; `/dev/shm` too, for `t=t` only, on Linux),
  or a directory of `images.kitty_file_allow_list`. Only temporary files are deleted, and only below a
  temp directory;
- no component of the path below that root is hidden (starts with `.`), and no symbolic link below
  the root is followed;
- it is a regular file, owned by the user Contour runs as, with a single link (so that a hard link
  cannot reach a file elsewhere), and no larger than one direct transmission may be;
- it is not below `/proc`, `/sys` or `/dev` (except `/dev/shm` as above).

Every refusal, whatever its reason — missing, outside the roots, the wrong kind of file, too large,
undecodable, or a medium not supported — is the same reply, so that a program learns nothing about a
file it may not read:

```
ESC _G i=<id>;EBADF:file not readable ESC \
```

### Who may read

A file is read where the program that named it runs, so file media are granted only to output that
comes from a local program. They are refused:

- in a session started from an SSH profile;
- in a tmux control-mode pane;
- in a daemon's replica of a session, and for bytes a client injects into a session;
- while the terminal's context (OSC 3008) says the output comes from another host;
- while the foreground process is a remote shell client (`ssh`, `mosh`, `et`, `telnet` and the like),
  or while the terminal cannot tell which process that is.

With `contour daemon`, the daemon reads the files: its own configuration decides, and a client can
neither widen nor narrow what a daemon reads. A daemon on another machine reads that machine's files,
which is where a program running there wrote them.

On Windows, file media are refused for now.

## Configuration

```yaml
images:
    # Whether kitty graphics may read files a local program names (t=f, t=t).
    kitty_file_transmission: true
    # Directories, besides the temp directories, that t=f may read files below. A leading ~ is your
    # home directory; a directory that would open all of it (/, ~ or above) is ignored.
    kitty_file_allow_list: ['~/.claude/plugins/data']
```

The default allow-list is where Claude Code's plugins keep what they render. The window reads its
configuration as each session starts, so an edit reaches the sessions opened after it; a daemon reads
its configuration once, when it starts.

## Claude Code

Claude Code shows pictures — Mermaid diagrams via the rich-terminal plugin, and anything a plugin
draws with its `<Image>` element — in terminals it knows to speak placeholders. Until Contour is on its
list, set `CLAUDE_CODE_FORCE_TERMINAL_IMAGES=1` in the environment of the session.

## Not yet supported

- **tmux control mode.** Placeholders reach a tmux pane through tmux's passthrough, and a file named
  there would be read on the machine running Contour while the tmux server may be elsewhere; such panes
  refuse file media until this is designed.
- **Windows file media.** The rules for file owners, hidden files and reparse points are not defined
  there yet.
