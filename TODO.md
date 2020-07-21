### Known Bugs

- PRIO-0: use signed int for `cursor_pos_t` and use negative numbers (maybe?) for accessing savedLines
- PRIO-1: wide cells don't show wide cursor (see README.md)
- alsamixer shows x instead of proper bars
- hot reloading of font family does not work
- mouse wheel: if configured action was executed, don't forward mouse action to terminal. example: alt+wheel in vim
- OpenFileManager action is missing impl, use xdg-open for that

- U+26A0 width = 1, why 1 and not 2? cursor offsetting glitch between contour and rest of world
	https://www.unicode.org/reports/tr11/tr11-36.html#Recommendations
	Chapter 5 (Recommendations), last bullet point!
	- provide a config option? (compile/run time?)

- [Windows] exit-watcher: quit the view/window when terminal app exits but report and wait for key press
      enter|escape if terminal app did exit too quickly (configurable: 5 seconds default)
      e.g. `alert_on_quick_termination: 5 seconds`

### Usability Improvements

- fade screen cursor when window/view is not in focus
- upon config reload, preserve currently active profile
- curly underline default amplitude too small in smaler font? (not actually visible that it's curly)
- hyperlink-opened files in new terminal should also preserve same profile
- double/tripple click action should heppen on ButtonPress, not on ButtonRelease.
- reset selection upon primary/alternate screen switch
- respect propotion of colored (emoji) glyph (y-offset / bearing)?

### QA: Refactoring:

- terminal::Mode to have enum values being consecutively increasing;
  then refactor Modes to make use of a bitset instead; vector<bool> or at least array<Mode>;
- SaveCursor/RestoreCursor greatly shows that the cursor is more than just a position, but
  also contains SGR, autoWrap, origin, character settings, selective erase attrs, SS2/SS3 states.
  Therefore we could refactor that into its own struct that isolates all of these attributes.
- savedLines screen history should be paged for 2 reasons (performance & easy on-disk swapping)
    - implement on-disk paging on top of that.
- Let go of `unsigned` (including `size_t`) unless it is meant for bit-pattern
- Use smallest integer type that is required to fit the
- Make use of MagicEnums
- Make use of the one ranges-v3
- yaml-cpp: see if we can use system package instead of git submodule here
- Functions: move C0 into it too (via FunctionCategory::Control)
- Functions: move OSCs into it too
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

### CI related

- CI: Finish release CI action (windows, osx, linux)
- CI: (release): ArchLinux: https://github.com/marketplace/actions/arch-linux-pkgbuild-builder-action
- CI: (release): to auto generate executables / packages [WINDOWS].
- CI: (release): to auto generate executables / packages [OSX].
- CI: (release): action: flatpak package
- CI: (release): action: Fedora package

### Features

- Evaluate Shell Integration proposals from: http://per.bothner.com/blog/2019/shell-integration-proposal/
- [Tab/Decoration styling](https://gitlab.gnome.org/GNOME/gnome-terminal/-/issues/142)
- OSC 7: [Set/unset current working directory](https://gitlab.freedesktop.org/terminal-wg/specifications/-/merge_requests/7)
- OSC 6: [Set/unset current working document WTF?](https://gitlab.freedesktop.org/terminal-wg/specifications/-/merge_requests/7)
- OSC 777: OSC Growl
- OSC 777: Windows Toast
- decorator: CrossedOut (draw line at y = baseline + (hight - baseline) / 2, with std thickness again
- decorator: Box (definitely!)
- decorator: Circle (maybe skip?)
- curly line thickness should adapt to font size
- Windows Font Matching (fontconfig equivalent?) - https://docs.microsoft.com/en-us/windows/win32/directwrite/custom-font-sets-win10
- mouse shift-clicks
- DCS + q Pt ST
- DCS + p Pt ST
- INVESTIGATE: is VT color setting for CMY and CMYK supported by other VTEs (other than mintty?)? What apps use that?
- create own terminfo file (ideally auto-generated from source code's knowledge)
- "Option-Click moves cursor" from https://www.iterm2.com/documentation-preferences-pointer.html
