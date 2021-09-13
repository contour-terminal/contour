### 0.2.1 (unreleased)

- Reverts change from 0.2.0: "Changes behaviour when receiving `U+FE0E` (VS15) to not enforce the width of 1 but leave it as is (usually 2). This seems to match what the web browser is doing, too."
- Adds some more tmux-extension entries to the terminfo database that are supported by contour (`Ss`, `Se`, `Cs`, `Smol`, `Smulx`, `Setulc`).
- Adds `Sync` capability entry to terminfo file.
- Do not force OpenGL ES on Linux anymore.
- Fixes CPU load going up on mouse move inside terminal window (#407).
- Fixes terminfo entries accidentally double-escaping `\E` to `\\E` (#399).
- Fixes RGB color parsing via ':2::Pr:Pg:Pb' syntax and also adapt setrgbf & setrgbb accordingly.
- Fixes SEGV with overflowing (Sixel) images (#409).

### 0.2.0 (2021-08-17)

- Improved performance (optimized render loop, optimized grapheme cluster segmentation algorithm)
- Improves selection to better automatically deselect on selected area corruption.
- Fixes `ioctl(..., TIOCGWINSZ, ...)` pixel values that were only set during resize but not initially.
- Fixes mouse in VIM+Vimspector to also change the document position when moving the mouse.
- Fixes SGR decorations to use designated underline thickness and underline position.
- Fixes font glyph render in some corner cases where the rendered glyph did result in rectangle garbage on the screen.
- Fixes copying the selection containing trailing whitespaces.
- Fixes hard-reset with regards to default tab width.
- Fixes VT sequence `DECRQPSR` for `DECTABSR`.
- Fixes keyboard keys for `F1`..`F4` when pressed with and without modifiers.
- Fixes OSC 8 hyperlink rendering when scrolled.
- Fixes SGR 1 (bold) wrongly applied on indexed background colors.
- Fixes text shaping sometimes showing missing glyphs instead of the actual glyphs (by changing the last-resort font fallback mechanism to chape each cluster individually with its own font fallback).
- Preserve active profile when reloading config, and forces redraw after config reload.
- Changes config entry `profile.*.font_size` to `profile.*.font.size`.
- Changes config entries `scrollbar.*` to `profile.*.scrollbar.*`.
- Changes behavior of live configuration reload, which is not default anymore and must be explicitly enabled via CLI parameter `--live-config`.
- Changes behaviour when receiving `U+FE0E` (VS15) to not enforce the width of 1 but leave it as is (usually 2). This seems to match what the web browser is doing, too.
- Changes the CLI command line interface syntax.
- Removes `tab_width` configuration. Tab width cannot be configured anymore but remaints by default at 8.
- Adds basic automatically created archive for Arch Linux to the CI build artifacts page and release page.
- Adds support for bypassing the mouse protocol via Shift-click (configurable via `bypass_mouse_protocol_modifier`)
- Adds improved debug logging. via CLI flag `-d` (`--enable-debug`) to accept a comma seperated list of tags to enable logging for. Appending a `*` at the end of a debug tag will enable all debug tags that match prefix its prefix.  The list of available debuglog tags can be found via CLI flag `-D` (`--list-debug-tags`).
- Adds support for different font render modes: `lcd`, `light`, `gray`, `monochrome` in `profiles.NAME.font.render_mode` (default: `lcd`).
- Adds support for different text render engines: `OpenShaper`, `DirectWrite` and `CoreText` for upcoming native platform support on Windows (and later OS/X).
- Adds support for different font location engines: `fontconfig` (others will follow).
- Adds experimental text reflow.
- Adds OpenFileManager action to configuration.
- Adds terminal identification environment variables `TERMINAL_NAME`, `TERMINAL_VERSION_TRIPLE` and `TERMINAL_VERSION_STRING`.
- Adds config option `mode` to input modifiers for additionally filtering based on modes (alt screen, app cursor/keypad, text selection modes, ...).
- Adds config option `profile.*.terminal_id: STR` to set the terminal identification to one of VT100, VT220, VT340, etc.
- Adds config option `profile.*.maximized: BOOL` to indicate maximized state during profile activation.
- Adds config option `profile.*.fullscreen: BOOL` to indicate fullscreen state during profile activation.
- Adds config option `profile.*.font.strict_spacing: BOOL` to indicate that only monospace fonts may be used.
- Adds config option `profile.*.font.TYPE.weight: WEIGHT` and `profile.*.fonts.TYPE.slant: SLANT` options (optional) along with `profile.*.fonts.TYPE.family: STRING`.
- Adds config option `profile.*.font.TYPE.weight: WEIGHT` and `profile.*.fonts.TYPE.slant: SLANT` options (optional) along with `profile.*.fonts.TYPE.family: STRING`.
- Adds config option `profile.*.font.dpi_scale: FLOAT` to apply some additional DPI scaling on fonts.
- Adds config option `profile.*.font.builtin_box_drawing: BOOL` to use pixel-perfect builtin box drawing instead of font provided box drawing characters.
- Adds config option `profile.*.refresh_rate: FLOAT` to configure how often the terminal screen will be rendered at most when currently under heavy screen changes. A value of `"auto"` will use the currently connected monitor's refresh rate.
- Adds configuration's action `ToggleAllKeyMaps` to enable/disable intercepting and interpreting keybinds. The one that did toggle it will not be disabled.
- Adds configuration's action `ClearHistoryAndReset` to clear the history, and resetting the terminal.
- Adds VT sequence for enabling/disabling debug logging. `CSI ? 46 h` and `CSI ? 46 l` and CLI option `-d`.
- Adds VT sequence for querying/setting current font `OSC 50 ; ? ST` and `OSC 50 ; Font ST` (and `OSC 60 Ps Ps Ps Ps Ps ST` for a more fine grained font query/setting control).
- Adds VT sequence `CSI 18 t` and `CSI 19 t` for getting screen character size. Responds with `CSI 8 ; <columns> ; <rows> t` and  `CSI 9 ; <columns> ; <rows> t` respectively.
- Adds VT sequence to capture the current screen buffer `CSI > LineMode ; StartLine ; LineCount t` giving the respone back on stdin via `OSC 314 ; <screen buffer> ST`, and feature detection via `DA1` number `314`.
- Adds VT sequence `DECSNLS` for setting number of lines to display.
- Adds VT sequence `CSI Ps b` (`REP`) for repeating the last graphical character `Ps` times.
- Adds VT sequence `OSC 4 ; INDEX ; COLOR ST` for setting or querying color palette (if COLOR is `?` instead of a color spec).
- Adds VT sequence `OSC 104 ; INDEX ST` for resetting color palette entry or complete palette (if no (index is given).
- Adds VT sequence `DECCRA` to copy a rectangular area.
- Adds VT sequence `DECERA` to erase a rectangular area.
- Adds VT sequence `DECFRA` to fill a rectangular area.
- Adds VT sequence `CSI > q` (XTVERSION) to query terminal identification (name and version). Response comes as `DCS >| Contour ContourVersion ST`.
- Adds VT sequence `DECRQM` to request ANSI/DEC modes states (set / unset / not recognized).
- Adds new CLI command: `contour capture ...` to capture the screen buffer.
- Adds new CLI command: `contour set profile to NAME` to change the profile on the fly.
- Adds new CLI command: `contour generate terminfo output OUTPUT_FILE` to create a Contour terminfo file.
- Adds new CLI command: `contour generate config output OUTPUT_FILE` to create a new default config.
- Adds new CLI command: `contour generate integration shell SHELL output OUTPUT_FILE` to create the shell integreation file for the given shell (only zsh supported for now). Also adds a pre-generated shell integration file for Linux (and OS/X) to `/usr/share/contour/shell-integration.zsh`.
- Unicode data updated to version 14.0 beta. See https://home.unicode.org/unicode-14-0-beta-review.
- Adds support for building with Qt 6 (disabled by default).
- Adds support for building with mimalloc (experimental, disabled by default).

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

