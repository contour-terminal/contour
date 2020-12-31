### 0.1.2 (unreleased)

- ...

### 0.1.1 (2020-12-31)

- Fixes race condition when displaying image animations (e.g. gifs via sixel).
- Fixes `NewTerminal` action to also inherit the active configuration file.
- Fixes restoring cursor position in `RM ?1049`.
- Fixes `DECSTR` resetting saved-cursor state and active cursor-position.
- Fixes selecting text not being pushed into the selection-clipboard.
- Adds VT sequence `OSC 7` (set current working directory).
- Adds VT sequence `DCS $ p <name> ST` to change config profile name to `<name>`.

### 0.1.0 (2020-12-24)

- Available on all 3 major platforms, Linux, OS/X, Windows.
- Emoji support (-: 🌈 💝 😛 👪 :-)
- Font ligatures support (such as in Fira Code).
- Bold and italic fonts
- GPU-accelerated rendering.
- Vertical Line Markers (quickly jump to markers in your history!)
- Blurred behind transparent background when using Windows 10 or KDE window manager on Linux.
- Runtime configuration reload
- 256-color and Truecolor support
- Key binding customization
- Color Schemes
- Profiles (grouped customization of: color scheme, login shell, and related behaviours)
- Clickable hyperlinks via OSC 8
- Sixel inline images

