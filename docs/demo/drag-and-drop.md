# Drag and drop

Contour accepts drops on a pane. What happens depends on what you drag: a pane or a tab moves, files and
text are typed into the pane's shell ([below](#dragging-files-onto-a-pane)).

## Moving panes and tabs

You can pick up any pane, or any tab, and put it anywhere a terminal can live: elsewhere in its tab, in
another tab, in another window, in a tab of its own, or in a window of its own. The shells inside keep
running through the move. Nothing is restarted and nothing is lost: scrollback, the running program, the
cursor, the selection and any search move with the pane, and output keeps arriving while you drag.

### The two handles

| You pick up | By |
|-------------|----|
| a **pane** | its **header** (the buttons on its right keep their click). The header is shown inline in Comfortable density; in Minimal density it is shown while you hold the reveal modifier. |
| a **tab** | the tab itself, in any window's tab bar. |

A tab with a **single pane** is that pane: dragging it behaves exactly as dragging the pane does, including
the swap below. A pane whose header is hidden because the pane is too short to spare the row has no handle
of its own; drag its tab instead.

While you drag a pane, a translucent card (its header, with a grip and the pane's name, and its last few
lines) follows the pointer, and the pane's place shows a dashed outline reading "*name*'s place", so you
can see what will close up behind it. Dragging a tab keeps the tab's own drag image.

### Where you can drop

Every target works for both handles, unless the table says otherwise.

| # | Release on | What happens |
|---|------------|--------------|
| T1 | the **edge band** of a pane (the outer 20 % of each side) | the dragged pane or tab splits beside, above or below that pane, taking half of it. The same tab, another tab, or another window. |
| T2 | the **middle** of a pane, with a single pane dragged | the two panes **swap** places. |
| T3 | the **middle** of a pane, with a tab of several panes dragged | nothing: a whole layout cannot trade places with one pane. No ghost is drawn. |
| T4 | **hovering a tab** in any tab bar for 600 ms | that tab becomes the visible tab of its window, with the drag still in your hand, so T1 and T2 then apply to its panes. |
| T5 | a **tab**, released on it (its middle, clear of the gaps) | the dragged pane or tab is added to that tab, as a split to the right of the tab's active pane. |
| T6 | a **gap** of a tab bar: between two tabs, before the first, after the last, or in the empty bar | the dragged pane or tab becomes a **new tab** at that place. For a tab this is the reorder, or the move into another window's tab bar. |
| T7 | **outside every Contour window** | a **new window** with the dragged pane or tab as its only tab. |

What you will get is drawn before you let go:

- Over a pane, a ring in the accent colour marks the pane, and a **ghost** shows the part of it the dragged
  node would take: the half beside the edge you are nearest (a corner goes to the edge you are
  proportionally nearer to; a tie goes to left or right), or the whole pane for a swap. A label on the
  ghost says what happens: `Pane → split left`, `Pane → split right`, `Pane → split up`,
  `Pane → split down` or `Swap with <pane name>`. A dragged tab of several panes says `Tab → split left` and
  so on.
- Over a tab, the tab is outlined in the accent. If hovering would open it, a bar fills along the tab's foot
  during the 600 ms, and it stays full once the tab has opened.
- In a gap, a dashed, tab-shaped slot named for what you carry shows where the new tab will stand. Between two
  tabs, the tabs after the gap move aside to make room for it.
- Dropping a pane on **itself**, or on the empty place it left, does nothing, and nothing is drawn.
  Dropping a pane on the only other pane of its own tab is a legal swap or re-dock. Dropping a tab onto a
  pane of its **own** tab does nothing either.

**Hover-to-open never moves the wrong thing.** It opens only another tab: never the tab you are dragging,
never the tab the drag came from, and never a tab of a window that would refuse the drop.

**Gaps versus tabs.** Each boundary between tabs counts as a gap for 10 px to either side (at most a quarter
of a narrow tab), so a gap is easy to aim at and a tab keeps a middle to rest on. The tab strip does not
reorder by where you drop along it any more; you reorder by dropping in the gap you want.

### Out to a new window

Release outside every Contour window (T7) and the pane or tab opens as a window of its own, at the cursor
position when you let go, on the screen you released on. On Wayland, which does not tell an application where
the pointer is outside its windows, the window opens at the last position the drag was seen inside a Contour
window, on that window's screen, and the compositor places it as it places every Wayland window. The platform
cannot draw outside Contour's windows, so the drag image alone shows what is happening until you let go.

A tab of several panes behaves as it always has: it tears off into a new window. A pane dragged out of a tab
of several panes now does the same, and leaves its place to its sibling.

Press **Escape** during a drag to cancel it: nothing moves and no window opens, even when the pointer is
inside Contour. The same goes for a release inside one of Contour's windows that no target takes.

### What a move does

- The new split divides the target pane in half, exactly as the ghost showed. In the tab the pane left, its
  sibling takes over the space. A swap trades the two places, and each place keeps its size.
- Focus follows the moved pane into its new tab and window, and that window is raised. Zoom is cleared on
  both the tab it left and the tab it entered.
- A tab left empty closes, and so does a window left empty, **without asking**: a move loses no session,
  and the question exists to protect running sessions. Moving the last pane of the last tab of a window out
  closes that window; the moved shell keeps running, and Contour does not quit.
- A screen reader announces the move ("Pane moved", "Pane moved to new tab", "Pane moved to window *name*",
  "Panes swapped", "Pane moved to new window").
- Daemon-hosted layouts are saved as they are after the move, so [restoring a workspace](../persistent-sessions.md)
  brings the new arrangement back.
- If the shell in the pane you carry exits before you let go, the drop does nothing.

### Between windows

| Where the windows are | Moving a pane or tab between them |
|-----------------------|------------------------------------|
| two windows of the **same Contour process** | works, every target above |
| two windows hosted by the **same daemon**, even when different Contour processes show them (see [Persistent sessions](../persistent-sessions.md)) | works, every target above |
| a window of **another Contour process** that shares no daemon with this one | refused |
| a window whose daemon is older than this one's pane-move support | refused for panes; see below |
| a **tmux** window (`contour client --tmux`) | refused |

A refused target is not silent. The ring turns amber, and a note on the pane says why and names what to do
instead. Releasing there does nothing at all: it is accepted as a no-op, so it never tears off a window by
accident, and everything stays where it was. The three notes, under the title **Can't move a pane here**:

- another process: *This window belongs to another Contour process. Run Contour in daemon mode to move panes
  between any windows.*
- an older daemon: *This window's daemon is too old to move panes. Update Contour on the daemon side.*
- tmux: *Panes can't be moved into a tmux window yet.*

**Older daemons.** A daemon that predates pane moves keeps serving what it always did: reordering tabs,
moving a tab to another window's tab bar, tearing a tab off into a new window, and dropping a tab on a pane
of **its own window** to split it. Dragging a pane, and every tab target that needs more than those, shows
the second note above. Nothing is ever half done: a move happens completely or not at all.

## Dragging files onto a pane

Drop files from your file manager onto a pane, and their paths are typed at the cursor, escaped for
the program in the foreground, and not run. A trailing space lets you keep typing.

| Foreground program | Quoting |
|--------------------|---------|
| a known POSIX shell (bash, zsh, fish, sh, dash, ksh, ...) | each special character, and spaces, escaped with a backslash |
| PowerShell (`pwsh`, `powershell`) | single quotes, `'` doubled |
| `cmd` | double quotes |
| anything else: Windows, a pane hosted by the daemon, a multiplexer (`tmux`, `screen`, `zellij`), a transport (`ssh`, `mosh`, `docker exec`, `su`, `sudo`, `wsl`), nushell, elvish, vim | double quotes, and a name holding any character one of those shells reads as syntax is refused |

The text arrives as a paste, so an application that enabled bracketed paste receives it wrapped.
Web links are typed the same way as file names, each one a quoted word, so a link a web page chose cannot
run anything. Dropping plain text pastes it as text, as Ctrl+V does, except that text with a line break is
refused where the program has not enabled bracketed paste: without it each line would run as it arrives,
and the page that started the drag chose the text, not you.

Quoting assumes the drop starts a new word. If you have an **open quote** at the prompt (a `"` or `'` you
typed and have not closed), a dropped name can close it, and what follows runs as syntax. Contour cannot
see the state of your shell's line editor, so finish or clear the line before you drop a file onto it.

## What is refused

- A file name holding a control character (a newline, an escape, and the like) or a character that
  makes the typed text look other than it runs (a text-direction control such as U+200E or U+202E, U+2028, U+2029) refuses the
  whole drop with "Not inserted: a file name holds a control or text-direction character.", because such
  a name could run as a command in the shell.
- Under `cmd`, a name holding `%`, `"` or `!` cannot be quoted safely and is refused too.
- Where the shell is not known, a name holding any of ``$ ` ; & | < > ( ) ' " \ ! % ^`` is refused, and the
  window says its shell is not known.
- A name or link starting with `-` would be read as an option: a local file is typed as `./-name`, and a
  link (which has no path to put `./` in front of) is refused except for a POSIX shell, which gets `./` too.
- A pane whose program has ended ignores drops.
