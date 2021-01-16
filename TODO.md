# FRIDAY TODO
- [ ] [REFLOW] Grid: finish unit tests
- [ ] [Font]: refactor Font API. - hasColor should not determine whether a glyph is emoji or not
- [ ] [Font]: investigate in faulty glyph offsetting?
- [ ] [Font]: investigate in glyphs exceeding cell dimensions in X, Y, or X & Y.

### font related refactor
- [x] get underline position & thickness from font metrics
- [x] fix doubly underline
- [x] fix curly underline
- [ ] fix framed underline
- [ ] add debug option to render frame around every texture quad in text renderer
- [ ] add crispy::text::GlobalFontMetrics maybe

```
* and at start of debug-logging, dump initial state once.
* log debug-logging start/end events to the debug log, too.
```

# REFLOW
- [ ] Add changelog entry: "Adds experimental support for text reflow"
- [ ] "// TODO: use `crispy::split(string) -> vector<string_view>` here"
- [ ] zsh/prompt integration: `SM/RM ? <number>` to enable/disable reflow on current and newly created lines (ignored in alternate screen).
- [ ] Fix column grow to insert spaces in unwrap.
- [ ] handle wide characters
- [ ] handle images, maybe don't wrap lines that contain image cells?
- [ ] selection: to honor wrapped lines in word-select
- [ ] selection: to honor wrapped lines in line-select
- [x] autoWrap to make use of reflow (also add test case)
- [x] don't scroll to bottom on resize

# TODO

- [ ] REFACTOR: wrap row/col/width/height numbers into structs
- [ ] writing text to stdout that triggers AutoWrap marks this line as auto-wrapped, so that
  word-selection will continue selecting on the enxt line.
  This mechanism could be used for a future text reflow functionality, too.
- [ ] ?? config's default-profile should also mandate defaults for other profiles. ??
- [ ] move to profile: `word_delimiters`
- [ ] move to profile: `scrollbar.*`
- [ ] move to profile: `images.*`
- [ ] `input_mapping` becomes default, `profiles.NAME.overrides.input_mapping` is used for overrides/additions
- [ ] make sure `input_mapping` overrides can also remove mappings
- [ ] move to ranges-v3 (eliminating some crispy helpers)

### Features to be added to 0.2.0 milestone

- [ ] contour: provide `--mono` (or alike) CLI flag to "just" provide a QOpenGLWindow for best performance,
      lacking UI features as compromise.
- [ ] RENDERING: respect propotion of colored (emoji) glyph (y-offset / bearing)?

### Known Bugs / Odds

- [ ] "The impossible happened" in TerminalWidget
- [ ] SGR underline not visible when inverse is set
- [ ] Text shaping on emoji presentation modifiers seem to sometimes get it wrong.
- [ ] `Config::fullscreen` is uninitialized and seems to be a dead member. Revive then.
- [ ] Do not do pressure-performance optimization when in alt-buffer
- [ ] charset SCS/SS not well tested (i.e.: write unit tests)
- [ ] OpenFileManager action is missing impl, use xdg-open for that
- [ ] U+26A0 width = 1, why 1 and not 2? cursor offsetting glitch between contour and rest of world
	https://www.unicode.org/reports/tr11/tr11-36.html#Recommendations
	Chapter 5 (Recommendations), last bullet point!
	- provide a config option? (compile/run time?)

### Usability Improvements

- Images: copy action should uxe U+FFFC (object replacement) on grid cells that contain an image for text-based clipboard action
- Images: Selecting grid cells that contain an image should colorize/tint this cell.
- don't `throw` but send notifications to `Terminal::warning(...)` and `Terminal::error(...)`;
  These notifications can then be bubbles or overlay-text (or whatever) per terminal view.
- mouse wheel: if configured action was executed, don't forward mouse action to terminal. example: alt+wheel in vim
- I-beam cursor thickness configurable (in pt, properly scaling with DPI)
- cursor box thicknes configurable (like I-beam)
- fade screen cursor when window/view is not in focus
- upon config reload, preserve currently active profile
- curly underline default amplitude too small in smaler font? (not actually visible that it's curly)
- hyperlink-opened files in new terminal should also preserve same profile
- double/tripple click action should heppen on ButtonPress, not on ButtonRelease.
- reset selection upon primary/alternate screen switch

### QA: Refactoring:

- move `string TerminalWindow::extractLastMarkRange()` into Screen and add tests
- Refactor tests so that they could run automated against any terminal emulator,
  which requires special DCS for requesting screen buffer and states.
  Target could be a real terminal as well as a mocked version for headless testing libterminal.
- terminal::Mode to have enum values being consecutively increasing;
  then refactor Modes to make use of a bitset instead; vector<bool> or at least array<Mode>;
- savedLines screen history should be paged for 2 reasons (performance & easy on-disk swapping)
    - implement on-disk paging on top of that.
- Make use of MagicEnums
- Make use of the one ranges-v3
- yaml-cpp: see if we can use system package instead of git submodule here
- Functions: move C0 into it too (via FunctionCategory::Control)
- have all schedule() calls that require a color to directly pass calculated color
- flip OpenGL textures so that they're better introspectible in qrenderdoc
- font fallback into a list of fonts, iterate instead of recurse until success or done

### Quality of code improvements:

- QA: Reevaluate making more use of QtGui (and Qt fonts) in `terminal_view`.
- See if we can gracefully handle `GL_OUT_OF_MEMORY`
- QA/TEST: Ensure os/x rendering is working (wrt. @AYNSTAYN)
- QA/TEST: make decoration parameter configurable in contour.yml (incl. hot reload)
- QA/APIDOC: Document as much as possible of the public API and potential algorithms
- QA: unit test: InputGenerator: `char32_t` 0 .. 31 equals to A-Za-z (and the others) and modifiers=Ctrl
- QA: unit test for verifying the new function handling properly processes `CSI s` vs `CSI Ps ; Ps s`
- QA: Font ctor: if `FT_Select_Charmap` failed, we shouldn't throw? Happens to `pl_PL` users e.g.?
- QA: error messages needs improvements so that I can relate them to source code locations
- QA: CUB (Cursor Backward) into wide characters. what's the right behaviour?
- QA: positioning the cursor into the middle/end of a wide column, flush left side on write.
- QA: enable/disable Ligature by VT sequence (so only certain apps will / won't use it)
- PERF: Use EBO in OpenGL to further reduce upload size, since grid is always fixed until screen resize.

### VT conformance

- CSI Pl ; Pc " p (Set conformance level (DECSCL), VT220 and up.)

### Features

- normal-mode cursor (that can be used for selection, basic vim movements)
- [Tab/Decoration styling](https://gitlab.gnome.org/GNOME/gnome-terminal/-/issues/142)
- OSC 6: [Set/unset current working document WTF?](https://gitlab.freedesktop.org/terminal-wg/specifications/-/merge_requests/7)
- OSC 777: OSC Growl
- OSC 777: Windows Toast
- decorator: CrossedOut (draw line at y = baseline + (hight - baseline) / 2, with std thickness again
- curly line thickness should adapt to font size
- Windows Font Matching (fontconfig equivalent?) - https://docs.microsoft.com/en-us/windows/win32/directwrite/custom-font-sets-win10
- mouse shift-clicks
- `DCS + q Pt ST` (Request Termcap/Terminfo String (XTGETTCAP), xterm.)
- `DCS + p Pt ST` (Set Termcap/Terminfo Data (XTSETTCAP), xterm.)
- create own terminfo file (ideally auto-generated from source code's knowledge)
- INVESTIGATE: is VT color setting for CMY and CMYK supported by other VTEs (other than mintty?)? What apps use that?
- "Option-Click moves cursor" from https://www.iterm2.com/documentation-preferences-pointer.html
- TMUX control mode: https://github.com/tmux/tmux/wiki/Control-Mode
- Rethink an easily adaptable keyboard input protocol (CSI based)
  - should support any key with modifier information (ctrl,alt,meta,SHIFT)
- Evaluate Shell Integration proposals from: http://per.bothner.com/blog/2019/shell-integration-proposal/
