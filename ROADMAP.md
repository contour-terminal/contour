
Release 0.1.0
-------------

* [x] should be generally working with most main stream applications (vim, mc, bash, zsh, zsh-extensions, tmux, screen)
* [x] should have generally good complex unicode support (that WILL be breaking with other terminals and apps!)
* [x] should have good configurability (no UI)
* [x] Emoji: simple as well as complex emoji codepoint sequences, including ZWJ emojis should be properly rendered (this will break other non-conforming apps!)
* [x] Emoji: flag sequences (such as US or EU flag)
* [x] platform binaries available for: Windows 10, Linux (deb, flatpak), OS/X
* [x] mouse input
* [x] configurable via YAML file: profiles, color schemes, fonts, input mappings
* [x] clipboard copy'n'paste and alternate clipboard
* [x] EXT: clickable hyperlinks (OSC 8)
* [x] EXT: OSC 52, clipboard manipulation
* [x] EXT: synchronized output (#119)
* [x] EXT: Terminal notifications for Linux

Release 0.2.0
-------------

* [ ] VIEW: cursor moves should be smoothly transition from one position to another (at least for same-row switches if the distance is <= 3)
            otherwise fade out in source, and fade in at destination
* [ ] VIEW: OpenGL based fade-transition when switching main/alt buffers
* [ ] VIEW: either "Good Image Protocol" or Sixel graphics support
* [ ] EXT: File transport (via `OSC 1337 File`
* [ ] Double-width/double-height character styles
* [ ] VIEW: output folding (based on vertical line markers) with actions to fold/unfold
* [ ] VIEW: audio bell
* [ ] VIEW: visuel bell (maybe use GLSL for a nice pulse-alike feedback)
* [ ] FONT: confgiurable font override for ranges of single codepoints
* [ ] FRONTEND: ability to disable ligatures rendering for some terminal programs (such as htop)
* [ ] FRONTEND: preview tooltips for OSC 8 hyperlinks, and images (if local)
* [ ] Terminal notifications: Windows Toast
* [ ] Terminal notifications: OSX (Growl)

Release 0.3.0
-------------

* [ ] UI: hot reloading shaders, if loaded from disk
* [ ] UI: show scrollbar
* [ ] UI: show minimap (alternative to scrollbar)
* [ ] VIEW: auto scroll when selecting beyond viewport

Release 0.x.0
-------------

* [ ] UI: tabs
* [ ] UI: tiling views
* [ ] UI: GUI window(s) can be detached into daemon mode and reattached later via GUI *and* via TUI (like tmux)

Release 1.0.0
-------------

* [ ] GUI configuration dialog
* [ ] screen recording (and replay tool)

