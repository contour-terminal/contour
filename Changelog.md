### 0.2.0 (unreleased)

- Fixes SGR decorations to use designated underline thickness and underline position.
- Fixes font glyph render in some corner cases where the rendered glyph did result in rectangle garbage on the screen.
- Fixes copying the selection containing trailing whitespaces.
- Fixes hard-reset with regards to default tab width.
- Fixes VT sequence `DECRQPSR` for `DECTABSR`.
- Preserve active profile when reloading config, and forces redraw after config reload.
- Changes config entry `profile.*.font_size` to `profile.*.font.size`.
- Changes behavior of live configuration reload, which is not default anymore and must be explicitly enabled via CLI parameter `--live-config`.
- Adds VT sequence for enabling/disabling debug logging. `CSI ? 46 h` and `CSI ? 46 l` and CLI option `-d`.
- Adds improved debug logging.
- Adds support for different font render modes: `lcd`, `light`, `gray`, `monochrome` in `profiles.NAME.font.render_mode` (default: `lcd`).
- Adds VT sequence for querying/setting current font `OSC 50 ; ? ST` and `OSC 50 ; Font ST` (and `OSC 60 Ps Ps Ps Ps Ps ST` for a more fine grained font query/setting control).
- Adds experimental text reflow.
- Adds OpenFileManager action to configuration.
- Adds config option `profile.*.maximized: BOOL` to indicate maximized state during profile activation.
- Adds config option `profile.*.fullscreen: BOOL` to indicate fullscreen state during profile activation.
- Adds config option `profile.*.fonts.only_monospace: BOOL` to indicate that only monospace fonts may be used.
- Adds config option `profile.*.fonts.TYPE.weight: WEIGHT` and `profile.*.fonts.TYPE.slant: SLANT` options (optional) along with `profile.*.fonts.TYPE.family: STRING`.
- Adds terminal identification environment variables `TERMINAL_NAME`, `TERMINAL_VERSION_TRIPLE` and `TERMINAL_VERSION_STRING`.
- Adds config option `profile.*.fonts.TYPE.weight: WEIGHT` and `profile.*.fonts.TYPE.slant: SLANT` options (optional) along with `profile.*.fonts.TYPE.family: STRING`.

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

