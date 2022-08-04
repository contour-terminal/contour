### 0.3.3 (unreleased)

- Adds vim-like `scrolloff` feature to normal mode cursor movements to ensure a line padding when scrolling up/down.
- Adds support for HSL colorspace in Sixel images.
- [Linux] Changes the .desktop file name and icon file name to conform to the flatpak recommendations.
- [Linux] Provide an AppStream XML file.
- [Linux] Drop KDE/KWin dependency on the binary by implementing enabling blur-behind background manually.
- [Linux] Adds support for blur-behind window on GNOME shell (Please read https://github.com/aunetx/blur-my-shell/issues/300 for further details if in trouble).
- Internal: Y-axis inverted to match GUI coordinate systems where (0, 0) is top left rather than bottom left.
- Fixes logging file toggle.

### 0.3.2 (2022-07-07)

- Fixes writing to a non-empty line sometimes destroying the contents of that line (#702).
- Fixes underline decoration for wide character cells.
- Fixes SGR 8 (Conceal/Hidden) attribute doesn't work as expected (#699).
- Fixes Synchronized Updates (`SM/RM ? 2026`) sometimes lagging behind in rendering.
- Fixes SGR and text breakage when altering charsets via `ESC ( 0` VT sequence (#661).
- Fixes SEGV when closing the terminal via GUI close button.
- Fixes scrolling in alt-screen.
- Fixes VT sequence for setting indexed color from palette.
- Fixes some config false positives error messages.
- Fixes command line arguments parser handling of `--` for switching to verbatim mode (#670).
- Fixes rendering of U+E0B2  in pixel-perfect box drawing mode (#707).
- Fixes rendering of cursor sometimes being almost invisible when foreground and background colors are similar/equal (#691).
- Fixes line based selection sometimes not selecting the full line when wrapped over multiple lines.
- Fixes warning message on terminal's console output when enabling blurred background images.
- Fixes Win32 command output: Attaches to parent console if present, so typing `contour help` in a terminal actually shows something.
- Changes `XTSMGRAPHICS` to match implementation of xterm *exactly* when querying sixel image limits, to be capped at terminal viewport dimensions (#656).
- Changes CLI syntax for `contour parser-table` to `contour generate parser-table`.
- Implements UTF-8 encoded mouse transport (`CSI ? 1005 h`)
- Improved vi-like input modes.
  - Fixed the text cursor not being visible during selection.
  - Don't leak but actually support cursor keys up/down/left/right/page-up/page-down.
  - Added `vip`, `vap` (and `yip` / `yap`) motions.
  - Adds `^` motion.
  - When being in normal mode, pressing `a` also puts you back to insert mode.
  - Properly show cursor if it was hidden in insert mode and restore visibility & shape when going back to insert mode.
  - `<S-K>` and `<S-J>` don't just move the cursor up/down but also move the terminal's viewport respectively (inspired by tmux).
  - `<S-H>` and `<S-L>` to move cursor to the current viewport's page top/bottom (inspired by tmux).
  - and more...
- Adds new config option `profile.*.vi_mode_highlight_timeout` and `colorscheme.*.vi_mode_highlight` and adds cell highlighting on yank (#669).
- Adds support for running on ARMv8 platform with crypto extensions (#611).
- Adds back support OpenGL ES (3.1).
- Adds E3 capability, so `clear` now defaults to clearing screen and scrollback (#693).
- Adds specialized PTY implementation for Linux operating system utilizing OS-specific kernel APIs.
- Adds basic support for Indicator status line and their VT sequences `DECSASD` and `DECSSDT`, and `DECRQSS` has been adapted (#687).
- Adds configuration option `profiles.*.status_line.display` to be either `none` or `indicator` to reflect the initial state of the status line (more customizability of the Indicator status-line will come in future releases).
- Adds new action `ToggleInputProtection` to protect terminal application against accidental input (#697).
- Adds configuration options `logging.enabled` as well as `logging.file`.
- Adds VT sequences `XTPUSHCOLORS`, `XTPOPCOLORS`, `XTREPORTCOLORS` (#714).
- Adds CLI syntax `contour info vt` to print list of supported VT sequences (#730).
- Adds VT sequence `DECPS` (#237).
- Adds new config option `colorscheme.selection.foreground_alpha` and `colorscheme.selection.background_alpha` to enable somewhat more eye-candy visual looks on text selection.
- Extends config option `colorscheme.selection.foreground` and `colorscheme.selection.background` to also accept `CellForeground` and `CellBackground` as value.

### 0.3.1 (2022-05-01)

- Adds Vi-like input modes for improved selection and copy'n'paste experience.
- Adds contour executable to search path for spawned shell process on OS/X and Windows.
- Adds customizability to dim colors (#664).
- Adds the profile configuration option: `draw_bold_text_with_bright_colors`.
- Fixes `CSI K` accidentally removing line flags, e.g. line marks (#658).
- Fixes wrong-spacing rendering bug on some lines.
- Fixes assertion on font resize when a (Sixel) image is currently being rendered (#642).
- Fixes assertion on too quick shell terminations (#647).
- Fixes fallback shell execution on UNIX-like systems when the reuqested shell cannot be spawned (#647).
- Fixes selection being wrongly rendered when viewport is scrolled (#641).
- Fixes full-line selection not properly injecting linefeeds between the lines.
- Changes behaviour of full-line selection to include a trailing linefeed for the last line (#641).
- Changes behaviour of bold text to by rendered using normal colors by default (was forced to bright before, and is now configurable via `draw_bold_text_with_bright_colors`).

### 0.3.0 (2022-04-18)

**TL;DR** Many thanks to all for the great support and feedback. This release
marks a huge milestone for me especially with regards to VT backend performance(!),
improved rendering architecture, and a more complete list of pixel perfect box drawings.

- Fixes installation from `.deb` (missing terminfo dependency)
- Fixes PTY write race condition.
- Fixes VT sequence `DECFI`.
- Fixes VT sequence `ICH` (#559).
- Fixes VT sequence `OSC 4`'s response.
- Fixes VT sequence `OESC 4` to also support setting color via `#RRGGBB`.
- Fixes VT sequence extension: capture buffer (#493).
- Fixes `DECRC` with respect to `DECSTBM` enabled and `DECOM` being inverted interpreted.
- Fixes `XTGETTCAP` (#582).
- Fixes `SU` in combination with `DECLRM` (#593).
- Fixes an assertion in text renderer.
- Fixes wrongly advertising DEC locator mode (it is not supported).
- Improved VT backend performance (#342).
- Improved text selection behaviour.
- Improved detection of runtime DPI changes for KDE (Plasma) desktop environment.
- Improves Window manipulation VT sequence for saving/restoring window title (`CSI 22 ; Ps t`, `CSI 23 ; Ps t`)
- Adds pixel-perfect box-drawing for U+EE00 .. U+EE05 for progress bar glyphs as first introduced by Fira Code (#521).
- Adds pixel-perfect box-drawing for U+E0B0, U+E0B2, U+E0BA (Triangle Powerline glyphs).
- Adds preliminary implementation of `DA3` VT sequence.
- Adds new configuration option `mouse_block_selection_modifier`.
- Adds new configuration option `profiles.*.copy_last_mark_range_offset` (default `0`) to adjust where to start looking upwards for the `CopyPreviousMarkRange` action. This is useful for multi-line prompts.
- Adds new configuration option `platform_plugin`.
- Adds new configuration option `renderer.backend` for explicitly setting renderer to one of: `OpenGL`, `software`, `default`.
- Adds new configuration option `renderer.tile_hashtable_slots` to configure texture atlas hashtable capacity.
- Adds new configuration option `renderer.tile_cache_count` to configure texture atlas cache capacity.
- Adds new configuration option `renderer.tile_direct_mapping` to enable/disalbe texture atlas direct mapping.
- Adds new colorscheme setting `background_image.path` and `background_image.opacity` to optionally render a terminal background image.
- Adds stdout-fastpipe support on new shell file descriptor `3` and exposing availability via environment variable `STDOUT_FASTPIPE` (with value `3`). When writing to this file descriptor rather than to stdout (file descriptor `1`), the full performance of Contour can be explored.
- Adds new configuration option `pty_buffer_size` for tweaking the new size in bytes for the PTY buffer objects.
- Adds `mock` font locator (mostly interesting for advanced use for CI testing).
- Adds VT sequence `SM ? 8452 h` / `RM ? 8452 l` for enabling/disabling sixel cursor placement conformance (xterm extension).
- Adds SGR-Pixels support, VT sequence `SM ? 1016 h` / `RM ? 1016 l` (#574).
- Adds VT sequence DECCARA, Change Attributes in Rectangular Area, with the extension that it applies to *all* SGR attributes. (#15).

### 0.2.3 (2021-12-12)

**Important: It is recommended to also use the latest `contour` termcap file if you are already using one
from a previous release.**

- Fixes Sixel image rendering when scrolling is needed and ANSI cursor is not on left margin.
- Fixes Qt-related CLI options that that were largely ignored.
- Fixes crash caused by VT sequence PM and SOS (#513).
- Fixes parsing VT sequence RGB color parsing for cell decoratioins (e.g. underline).
- Fixes double-underline to not look like a very thick line on small font sizes.
- Applies antialiasing to curly underline.
- Changes `contour` exit code to reflect the shell's exit code of the last closed window.
- Improves text cursor rendering and extends cursor configuration accordingly (#526).
- Improves visual appearance of dotted underline SGR.
- Adds CLI option `terminal early-exit-threshold SECS` (defaulting to 6) to only report and wait if the process did exit below this threshold seconds.
- Adds CLI option `terminal dump-state-at-exit` to auto-dump internal state at exit.
- Adds support for CoreText for matching font descriptions and font fallback (#479).
- Adds support for font feature settings. This is currently only implemented for `openshaper`, not yet for `dwrite` (#520).
- Adds pixel-perfect box-drawing for U+E0B4, U+E0B6, U+E0BC, U+E0BE (some [Powerline extended codepoints](https://github.com/ryanoasis/powerline-extra-symbols#glyphs)).

### 0.2.2 (2021-11-19)

- Fixes input mapping containing `Control` modifier.
- Fixes input mapping mode `Select` being ignored.
- Fixes Modifier+Enter input mapping not being catched.
- Fixes slant detection for DirectWrite.
- Changes `DECCOLM` to only switch back to 80 when it was 132 before.
- Adds new config option `spawn_new_process` to define new terminal window behavior.
- Adds action `CancelSelection` to allow actively canceling selection via input mappings.
- Adds key bindings to default configuration to allow simply pressing Ctrl+C/Ctrl+V (without Shift modifier) when an active selection is present.
- Adds process current working directory on macOS.
- Adds `contour license` CLI command to show project license but also an overview of all dependencies.
- Adds a proper CLI to `bench-headless`. Building the headless benchmark tool is not shipped by default.

### 0.2.1 (2021-11-14)

- Reverts change from 0.2.0: "Changes behaviour when receiving `U+FE0E` (VS15) to not enforce the width of 1 but leave it as is (usually 2). This seems to match what the web browser is doing, too."
- Adds support for loading terminal color schemes from an exernal file, such as `~/.config/contour/colorschemes/onedark.yml` for the scheme `onedark` (file file format is simply a sub-tree of how colors can be specified inline).
- Adds some more tmux-extension entries to the terminfo database that are supported by contour (`Ss`, `Se`, `Cs`, `Smol`, `Smulx`, `Setulc`).
- Adds `Sync` capability entry to terminfo file.
- Adds many more pixel-perfect graphical characters: `23A1`..`23A6`, `2580`..`2590`, `2594`..`259F`, `1FB00`..`1FB3B`, `1FB3C`..`1FBAF`, `1FBF0`..`1FBF9`.
- Adds support for building with embedded FreeType and HarfBuzz (experimental, disabled by default).
- Adds a shell early-exit-guard to not instantly close the terminal window but print a message instead and wait for any key press in order to close.
- Adds missing config option `read_buffer_size` to default `contour.yml`.
- Adds new config option `reflow_on_resize` to mandate whether or not text reflow is enabled on primary screen. If this option is false, it cannot be enabled programmatically either.
- Adds new config option `on_mouse_select` to decide what action to pick when text has been selected (copy to clipboard or copy to selection-clipboard, or do nothing)
- Unicode data updated to version 14.0 (release). See [Announcing The Unicode® Standard, Version 14.0](https://home.unicode.org/announcing-the-unicode-standard-version-14-0).
- Do not force OpenGL ES on Linux anymore.
- Changes default (Sixel) image size limits to the primary screen's pixel dimensions (#408).
- Changes font locator engine default on Windows to DirectWrite (#452).
- Changes tcap-query feature from experimental to always enabled (not configurable anymore).
- Automatically detect if `contour` or `contour-latest` terminfo entries are present use that as default.
- Fixes VT sequences that cause a cursor restore to sometimes crash.
- Fixes terminfo installation path on OS/X and tries to auto-set `TERMINFO_DIRS` to it on startup (#443).
- Fixes terminfo entry `pairs`.
- Fixes SGR 24 to remove any kind of underline (#451).
- Fixes font fallback for `open_shaper` where in rare cases the text was not rendered at all.
- Fixes CPU load going up on mouse move inside terminal window (#407).
- Fixes terminfo entries accidentally double-escaping `\E` to `\\E` (#399).
- Fixes RGB color parsing via ':2::Pr:Pg:Pb' syntax and also adapt setrgbf & setrgbb accordingly.
- Fixes SEGV with overflowing (Sixel) images (#409).
- Fixes XTSMGRAPHICS for invalid SetValue actions and setting Sixel image size limits (#422).
- Fixes internal pixel width/height tracking in VT screen, which did affect sizes of rendered Sixel images (#408).
- Fixes configuring a custom shell on OS/X (#425).
- Fixes off-by-one bug in builtin box drawing (#424).
- Fixes assertion in text renderer with regards to colored glyphs.
- Fixes Sixel background select to support transparency (#450).
- Fixes session resuming on KDE desktop envionment which is respawing all Contour instances upon re-login but failed due to invalid command line parameters (#461).
- Fixes Meta+Key keyboard inputs being ignored.
- Changes DECSDM such that it works like a real VT340; also xterm, as of version 369, changed that recently (#287).
- Adds context menu support for KDE.

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

