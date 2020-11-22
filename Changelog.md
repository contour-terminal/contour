### 0.1.0 (unreleased)

#### prerelease 4

- Fixes Single-click into the window accidentally starting a selection.
- Fixes font family change in live config reload.
- Fixes config `initial_working_directory`'s' ~ (tilde) expansion failing to chdir, if followed by a `/`.
- Refactoring various internal parts to improve code quality.
- Adds scrollbar and configuration for scrollbar.
- Adds input mapping for `Shift+Tab` (`CSI Z`)
- Adds VT sequence `CHT`.

#### prerelease 3

- Fixes missing .exe files in windows installer

#### prerelease 2

- `initial_working_directory` config option added
- Selection improved, and `selection.foreground` and `selection.background` config options added to color profile
- VT mouse protocol fixes in `vim`
- Debian packages now contain application image and `.desktop` file
- Implements `OSC 117` and `OSC 119` (resetting selection text/background color to application defaults).

#### General Features

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

