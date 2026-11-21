# Kitty Unicode placeholders and file media

The user-facing description is [Kitty graphics: Unicode placeholders and file media](../vt-extensions/kitty-graphics.md).
This page records the two design decisions that are not obvious from the code, and why they were
made.

## A flag at print time, a resolver at the end of the batch

A placeholder cell's meaning is spread over its text, its foreground colour, its underline colour and
up to three combining diacritics, and it may depend on its left neighbour (kitty infers an omitted
row or column from it). None of that is complete when the base character is printed: the diacritics
follow it, they may arrive in a later read from the PTY, and the image or virtual placement it names
may be transmitted after the cells are printed.

Resolving per printed cell would therefore resolve against a cell that is not finished yet, and
would put image lookups on the print path every character takes. Instead:

- **The print path only flags.** When a cluster's base code point is `U+10EEEE`, the line gets
  `LineFlag::ImagePlaceholders` and the screen notes that a scan is pending. Ordinary text pays one
  comparison.
- **The batch end resolves.** `Screen::resolveKittyPlaceholders()` runs once per parse batch, over the
  flagged lines, through a **pure** per-line resolver in `graphics/KittyPlaceholders.hpp`: cells in,
  runs of image cells out, with kitty's left-neighbour inference. The screen then attaches or drops
  the fragments. A cell split across reads is right by construction, because the resolver only ever
  sees whole lines after the batch.
- **A late image re-runs the same code.** Creating a virtual placement, re-transmitting an image or
  resizing the page makes the next scan walk the flagged history lines too
  (`PlaceholderScan::Everywhere`); otherwise it walks the page and the rows scrolled out during the
  batch. The flag is what bounds it: unflagged history is never scanned. A line keeps the flag while it
  holds unresolved placeholders, so a picture printed before its image arrives appears when it does.

Because the resolver is pure, it is tested as a table (colours, diacritics, inference, unknown ids,
out-of-span cells) without a screen; the screen tests only cover the wiring and the bounds.

The flag and the fragments are line state, so the live upgrade (`state/`) and the daemon's mirror carry
them without knowing about placeholders: the mirror receives image cells, each naming a placement
from the Delta's placement table (protocol v5), and renders them like any other image.

## A file medium is judged by the descriptor, not by the path

`t=f` and `t=t` let any program that can write to the terminal name a file for the terminal to read
— and for `t=t`, to delete. The terminal reads as the user, so a lax check is a confused deputy: a
link into `~/.ssh`, a hard link to a private key inside an allowed directory, a FIFO that blocks the
event loop, or a path swapped between the check and the open.

A check on the path followed by an `open` of the path has a window between the two that no amount of
checking closes. The read is therefore built around the file descriptor:

1. **A pure policy** (`graphics/KittyMediaPolicy.hpp`) judges the requested path lexically: absolute,
   below a root, no hidden component below it, not a system tree, no longer than `PATH_MAX` allows.
   For `t=t` it also decides the deletion: only a file whose full path carries the
   `tty-graphics-protocol` marker is deleted, as kitty has it; one without the marker is read and kept.
2. **The platform source** (`KittyMediumSource_posix.cpp`) opens the root, then walks one component at
   a time with `openat(O_NOFOLLOW)`: no symbolic link below a root is ever followed, so there is no
   later "real path" to compare against — the path walked *is* the real path. The final open is
   `O_NONBLOCK`, so a FIFO cannot stall the reader.
3. **The descriptor is judged** with `fstat`: a regular file, owned by the reading user, `st_nlink == 1`,
   within the size limit. Only then is it read, bounded by that limit.
4. **Deletion** (`t=t` only, and only below a temp root) goes through the walked parent's descriptor
   with `fstatat` and `unlinkat`, after checking that the entry is still the file that was read.

Every refusal, from any step, is the same reply (`EBADF:file not readable`), so that the protocol is
not an oracle for files outside the roots. The policy is supplied at construction from the
configuration (`images.kitty_file_*`) and the platform (temp roots, user id); it is given only to
terminals whose bytes come from a local process, and the parsing host's own configuration decides — it
is never part of a session's settings on the native protocol's wire, so a client cannot widen what a
daemon reads.

`t=s` (POSIX shared memory) is not supported: reading it requires unlinking the named object, which
would let any program writing to the terminal remove any shared memory object the user owns.
