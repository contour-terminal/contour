# Quick terminal

A global hotkey slides a terminal down from the top of the screen, from whatever application you are in, and the
same hotkey hides it again. It is full width and 40 % of the screen's height, on the screen the pointer is on,
in the Minimal chrome density.

It is off until you give it a hotkey: open **Settings › General › Quick Terminal**, press **Record** and press
the chord you want. A chord needs at least one of Control, Alt and Super, and cannot be one the macOS menu bar
keeps (such as Command-Q). Delete in the field removes it.

## The setting

```yaml
quick_terminal:
  hotkey: { mods: [Control], key: '`' }
  hide_on_focus_loss: true
```

| Key | Meaning |
|-----|---------|
| `hotkey` | The chord, spelled as an input mapping spells its chord. Unset by default. |
| `hide_on_focus_loss` | Hides the quick terminal when another window takes the focus. `true` by default; with `false` it stays until the hotkey is pressed again. |

Changing the hotkey takes effect at once, without a restart.

## Per platform

- **macOS:** the window is a non-activating panel, so the application you came from stays frontmost and keeps its
  menu bar while you type into the quick terminal.
- **Windows:** the hotkey is registered with the system. Another application that holds the chord makes the
  registration fail, and Contour says so.
- **Linux:** the hotkey goes through the desktop's GlobalShortcuts portal, which may ask you to confirm it. A
  desktop without that portal has no global hotkeys. Under Wayland a window cannot place itself, so the quick
  terminal opens where the compositor puts it and fades in.

With Reduce Motion on, the quick terminal fades in and out instead of sliding. So does it where another screen
is stacked above the one it opens on, since sliding in from above would show it on that screen. The quick
terminal never keeps Contour running while it is hidden: closing the last ordinary window leaves the process
running only while the quick terminal is on screen, and hiding it then quits. Closing its last tab ends it, and
the next press opens a new one. In daemon mode it is a window of this machine alone: its terminals run here and
the daemon never hosts, saves or restores them.
