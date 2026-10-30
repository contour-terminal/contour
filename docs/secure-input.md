# Secure input

While a program in a pane hides what you type, as `sudo` and `ssh` do when they ask for a password, Contour
shows a lock for that pane. On macOS it also turns on Secure Keyboard Entry for as long as that pane has
focus, which keeps other applications from reading your keystrokes.

## The setting

```yaml
secure_input: auto
```

| Value  | Meaning |
|--------|---------|
| `auto` | Lock and, on macOS, Secure Keyboard Entry while a pane asks for hidden input. |
| `off`  | Never. `false` and `no` read as `off`. |

A new configuration (`defaults_edition: native2026`) uses `auto`; the Classic edition keeps `off`. Reloading the
configuration applies the change at once: turning it off releases Secure Keyboard Entry immediately.

## What counts as a password prompt

A pane is locked when its terminal has **echo off and canonical (line) mode on**: the state `sudo`,
`readpassphrase(3)`, `ssh` and `read -s` put it in. A program that clears echo for its own screen, such as an
editor, `htop`, `less` or the line editor of your shell, also turns canonical mode off, so it shows no lock.
Ghostty reads the same two flags (`password_input` in its `termio/Exec.zig`); xterm has no automatic detection and
only a manual Secure Keyboard menu entry.

### What it cannot see

Contour looks at the terminal's flags and nothing else, so it misses a password reader that clears **both** echo
and canonical mode. Those readers show no lock and turn nothing on:

- `sudo` with `pwfeedback`, and sudo-rs with its feedback option
- `pinentry-curses`
- `sudo` and similar asked over `ssh`, where the prompt runs on another machine's terminal
- panes whose program runs on a host Contour cannot read flags from (an SSH pane, a Windows ConPTY pane)

For these, the lock is simply absent: a lock means the program hides the typing, and no lock means only that
Contour could not tell.

### How quickly it notices

Contour re-reads the flags when output reaches the pane, before each key and paste is sent, and on focus. For two
seconds after the pane's last output or key it also checks every 100 ms while the pane has focus, then stops: an
idle pane wakes nothing. A program that turns echo off only after more than two quiet seconds is noticed at the
first key you type. That key has already reached the system by then, so Secure Keyboard Entry protects the keys
after it, not that one. The opposite case leans safe: a program that turns echo back on silently, more than two
seconds after the last key or output, leaves the lock and Secure Keyboard Entry on until the next key or output.

Panes hosted by a daemon report the same state over the native protocol, so the lock works under
`daemon.mode: auto` as well.

## Where the lock shows

Wherever something showing the pane is on screen: in the pane's header, on its tab, in a chip in the title band, and,
when none of those is on screen, in a small pill over the pane's blank cells at the top right. The pill avoids the
prompt and yields to the link pill and the find bar; if a full-screen program leaves no blank place, it covers a few
cells rather than disappear.

The words beside the lock read **Secure input on** only for the pane the system's Secure Keyboard Entry protects: on
macOS, the focused pane of the front application. Every other locked pane (an unfocused split, a background tab) and
every other platform read **Input hidden**, which says only that the terminal hides what you type. Screen readers
announce the same words.
