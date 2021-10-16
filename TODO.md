# TODO(pr) requirements before merge

- [ ] ring: add negative and overflowing (r)iterator tests (should wrap around & succeed)
- [ ] frontend: scrolling up/down works as expected (currently overflowing and inverted)
- [ ] Terminal: Writing text, leading to page-scroll properly updates scrollbar.
- [ ] Terminal: Writing text, leading to page-scroll properly updates active selection.
- [ ] grid: revive logical line iterators, try to make them zero-copy
- [ ] notify on dirty screen regions should become an area-test: `is (x,y) within ((x0,y0), (x1,y1))`

# FIXMEs / CODEHALTH checklist

- [ ] double-click line select on empty line selects everything to the right - should maybe not select anything?
- [ ] SEGV handler should probably only use `backtrace_symbols_fd()`
- [ ] code health (get compiler warnings branch merged, have it compile-time optional `CONTOUR_PEDANTIC=OFF/ON`
- [ ] improve unit tests (InputGenerator)
- [x] CHECK: profile change upon shortcut
- [x] TEST: RenderBuffer ops testable (esp. 2026)?
- [ ] BUG? wrt RenderBuffer: maybe problem with vte emoji asset test? (one emoji)
- [ ] BUG? hidden scrollbar in alt screen causes render artifacts sometimes?
- [ ] BUG? eval need of `Terminal::shouldRender` and friends
- [ ] CHECK: CopyLastMarkRange seems to not copy last 2 lines instead (due to zsh double line prompt?) (- try with zsh single line prompts | - ensure unit tests for that)
- [ ] RENDER-BUG: "template..." is segmented as "template." and ".." but should be "template..." (or "template" and "...")
- [ ] BUG? showNotification: not working (because not connected, not ported)
- [ ] move scrollbar into profile
- [ ] dotted underline could be prettier. Not as circles but as squares (because circles w/o AA look bad)
- [ ] FIXME: `reset` resets screen size to 80x25, should remain actual one.
- [ ] BUG/SECURITY: DCS without ST is problematic (what are other TEs doing?)
- [ ] resizing font to HUGE and then moving back instantly (Ctrl+0) may cause SEGV b/c of word-wrap
- [ ] fix '🇯🇵' when surrounded with text (roflmao)
- [ ] contour-cli deb package (without terminal GUI)
- [ ] config option to disable reflow entirely
- [ ] `ls -l --color=yes /` with wrapping on a bg-colored file (vmlinuz...) will cause the rest of the line to be bg-colored, too. that's wrong. SGR should be empty.This problem only exists when not having resized yet.
- [ ] vim's wrap mode with multiline text seems to have rendering issues.
- [ ] debuglog: filter by logging tags (in a somewhat performant way), so the debuglog (when enabled) is not flooding.
- [x] Font: support DirectWrite backend
- [ ] Font: fix framed underline
- [x] Font: hasColor should not determine whether a glyph is emoji or not
- [x] logger: on win32 the function name is too verbose.
- [ ] SGR underline not visible when inverse is set
- [ ] Don't perform pressure-performance optimization when in alt-buffer
- [ ] charset SCS/SS not well tested (i.e.: write unit tests)
- [ ] walk through the source code and apply cleanups & coding style
- [ ] cleanup config (and contour.yml) from dead options (logging?)
- [ ] debug log must include software git sha (and version)

```
* and at start of debug-logging, dump initial state once.
* log debug-logging start/end events to the debug log, too.
```

### U+26A0

U+26A0 width = 1, why 1 and not 2? cursor offsetting glitch between contour and rest of world
https://www.unicode.org/reports/tr11/tr11-36.html#Recommendations
Chapter 5 (Recommendations), last bullet point!

- provide VT sequence to get/set unicode (width) conformance level (pre unicode 9, and unicode 9+)
- see https://gitlab.freedesktop.org/terminal-wg/specifications/-/issues/9

# Features

- [ ] trigger config reload via VT sequence (as SIGUSR1 won't work on windows).  This function is behind permission gate and triggers a popup dialog when set to "ask".
- [ ] move to profile: `word_delimiters`
- [ ] move to profile: `scrollbar.*`
- [ ] move to profile: `images.*`
- [ ] `input_mapping` becomes default, `profiles.NAME.overrides.input_mapping` is used for overrides/additions
- [ ] make sure `input_mapping` overrides can also remove mappings
- [ ] move to ranges-v3 (eliminating some crispy helpers)
- [ ] "The impossible happened" in TerminalWidget
- [ ] contour: provide `--mono` (or alike) CLI flag to "just" provide a QOpenGLWindow for best performance,
      lacking UI features as compromise.

### Usability Improvements

- ? Images: copy action should uxe U+FFFC (object replacement) on grid cells that contain an image for text-based clipboard action
- ? Images: Selecting grid cells that contain an image should colorize/tint this cell.
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

- Configuration: ability to disable ligatures globally (or enable selectively by unicode range?)
- respect aspect ratio of colored (emoji) glyph (y-offset / bearing)?
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
