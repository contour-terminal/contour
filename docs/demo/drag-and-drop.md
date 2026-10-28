# Drag and drop

Contour accepts drops on a pane. What happens depends on what you drag.

## Dragging a tab onto a pane

Drag a tab from the tab strip over the **outer edge** of a pane. A ghost shows the half of the pane
the tab will take, labelled for the side ("Tab → split right"). Release, and the pane splits: the
dragged tab's whole pane layout becomes the new half, with its shells still running. If that was
the only tab of its window, the window closes.

- Release in the **middle** of a pane, and the tab tears off into a new window, as it always has.
- A tab dropped on a pane of **its own tab** does nothing.
- In a window whose sessions run in the daemon (see [Persistent sessions](../persistent-sessions.md)),
  a tab from the same window splits through the daemon. A tab dragged between a daemon window and
  another window still tears off.

## Dragging files onto a pane

Drop files from your file manager onto a pane, and their paths are typed at the cursor, escaped for
the program in the foreground, and not run. A trailing space lets you keep typing.

| Foreground program | Quoting |
|--------------------|---------|
| bash, zsh, fish, sh, vim and others | each special character, and spaces, escaped with a backslash |
| PowerShell (`pwsh`, `powershell`) | single quotes, `'` doubled |
| `cmd` | double quotes |
| unknown (Windows, a pane hosted by the daemon, `ssh`, `wsl`, `sudo`) | double quotes, and a name holding any character one of those shells reads as syntax is refused |

The text arrives as a paste, so an application that enabled bracketed paste receives it wrapped.
Web links are typed the same way as file names, each one a quoted word, so a link a web page chose cannot
run anything. Dropping plain text pastes it as text, as Ctrl+V does.

## What is refused

- A file name holding a control character (a newline, an escape, and the like) or a character that
  makes the typed text look other than it runs (a text-direction control, U+2028, U+2029) refuses the
  whole drop with "Not inserted: a file name holds a control or text-direction character.", because such
  a name could run as a command in the shell.
- Under `cmd`, a name holding `%`, `"` or `!` cannot be quoted safely and is refused too.
- Where the shell is not known, a name holding any of ``$ ` ; & | < > ( ) ' " \ ! % ^`` is refused, and the
  window says its shell is not known.
- A pane whose program has ended ignores drops.
