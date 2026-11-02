# Keyboard input

How a key press becomes bytes for the program in the terminal. The pipeline has one place where a
platform is allowed to matter -- the normalizer -- and everything beneath it is platform-free. This page
gives the overview, says where each platform decision lives, and records the decisions behind the parts
that are not obvious from the code.

## The pipeline

```
QKeyEvent / QInputMethodEvent
   |  captureKeyEvent()            input/QtKeyCapture.cpp        (the only reader of a Qt key event)
   v
NativeKeyEvent                     plain data: recordable, replayable, no Qt in it
   |  KeyboardPlatform::normalize()  input/{Xkb,Win32,Mac}KeyboardPlatform.cpp
   v
KeyEvent                           a named key, or text; the semantic chord; Option side; app modifier
   |  routeKey() / routeChar()     input/KeyRouter.cpp           (user binding, built-in binding, else...)
   v
KeyRoute -> SessionInput           session/SessionInput.cpp, TerminalSessionKeyInput.cpp
   |  run the bound actions, or hand the key / character to the terminal's encoder (vtbackend)
   v
bytes for the PTY
```

Every `KeyboardPlatform` compiles on every host and is picked at runtime from a `KeyboardPlatformKind`
(`Xkb`, `Win32`, `Mac`), so each platform's rules run in the Linux test suite: the characterization table
(`session/KeyCharacterizationRows.hpp`) pins the bytes written for a native event of each kind, and the
per-platform tests drive the normalizers with recorded events.

### Where each platform decision lives

| Decision | Home |
|----------|------|
| What the Qt event holds, and the Windows lock-key sample (`GetKeyState`) | `input/QtKeyCapture.cpp` |
| The active native key layout (Carbon / Windows `ToUnicodeEx` / the live XKB keymap) | `input/KeyboardLayout.cpp`, `input/XkbLiveKeymap.cpp` |
| Which kind this build is | `config/KeyboardPlatformKind.hpp`, read once by the composition root |
| Option sides, ⌘ as the application modifier, the Option rule, input-method bypass (macOS) | `input/MacKeyboardPlatform.cpp` |
| AltGr versus Ctrl+Alt, lock keys (Windows) | `input/Win32KeyboardPlatform.cpp` |
| Layout-resolved OEM keys (Windows): a virtual key named through `ToUnicodeEx`, the US table behind it | `input/KeyboardLayout.cpp` |
| Keysym to codepoint, the unshifted key from the keymap (X11, Wayland) | `input/XkbKeyboardPlatform.cpp`, `input/KeysymTable.hpp`, `input/XkbKeymap.hpp` |
| Which chords are default bindings on which platform, and `Primary` | `config/DefaultBindings.cpp` (one table, with a platform column), `config/ModifierNames.hpp` |
| How a shortcut is drawn (`⇧⌘P` or `Ctrl+Shift+P`) | `command/Shortcut.*` |

Nothing below the normalizer asks which platform it runs on. The `Primary` modifier is resolved when the
configuration is read, against the kind the configuration was loaded for, and bindings are matched on the
resulting concrete chord.

### The lint gate

`scripts/check-input-platform-neutral.py` (CTest label `lint`, with a `--self-test`) fails when a platform
test -- `#if`, `#ifdef`, `#elif`, `defined(` naming `__APPLE__`, `_WIN32`, `_WIN64`, `__linux__`, `__unix__`
or `Q_OS_*` -- or the identifier `BuildKeyboardPlatformKind` or `BuildPaneChordHost` appears in the files
that sit beneath the normalizer: the key event and platform interface, the router, the key tables, the
session's key path and the default-binding table. Add a new file of that kind to the list in the script. The
composition root may use `BuildKeyboardPlatformKind`; the files that may test the platform are
`QtKeyCapture.cpp`, `KeyboardLayout.cpp`, `XkbLiveKeymap.cpp` and `KeyboardPlatformKind.hpp`.

### Latency

`normalize()` plus `route()` is budgeted at 1 microsecond at the 99th percentile on a release build, and a
single-codepoint key press allocates nothing. `input/KeyPipeline_bench_test.cpp` measures it
(`contour_gui_test "[bench]"`; set `CONTOUR_BENCH_ENFORCE=1` to make the budget a failure), and
`bench-headless keypress` is the end-to-end comparison against `master`.

### Recording what a platform delivers

Every key press and every input-method commit is captured as plain data (`NativeKeyEvent`) before anything
decides what it means. While the `gui.input` log category is enabled, each one is also logged in the
form `formatRecorded()` writes and `parseRecorded()` reads back:

    Captured key=0x41 mods=0x0 native=0x0 vk=0x0 scan=0x0 type=press origin=keyboard text=a

Nothing is formatted or allocated while the category is off. A tester who sees a wrong key enables it
(`contour --debug gui.input`), reproduces the key and sends
back the `Captured ...` lines with their platform, keyboard layout and the bytes the program received.
`parseRecorded()` turns such a line into a row for the characterization table. The text field is escaped
(`\x20` for a space, never a raw one), and a line holding a raw space in it is rejected.

### Known limitations

* **A chrome shortcut lookup on macOS cannot tell the Option sides apart.** `chordOfKeyPress()` rebuilds
  the press from a QML `KeyEvent`, which exposes no `nativeModifiers`, so with `option_as_alt` set to `left`
  or `right` the Option key reads as the side the setting does not name. Terminal input is unaffected.

* **Wayland has no live keymap.** Qt does not expose the compositor's keymap, so the unshifted key is read
  through the keysym table (below).
* **A rewrite of the XKB keymap on X11 is not followed** until Contour restarts (below).
* **The macOS input-method bypass is unverified on hardware.** The decision (`inputMethodRouteFor`) is
  table-tested, but whether Qt lets a held key skip the input method per keystroke has only been checked
  by reading Qt, not on a Mac.
* The macOS and Windows normalizers are pinned by recorded-event tests written from what Qt is believed to
  deliver; the macOS and Windows CI runners are what confirms those rows.

## Where the live XKB keymap comes from

The kitty keyboard protocol's `unshifted-key` field names the character a key types with no modifier
held. On X11 and Wayland a Qt key event carries the keysym the key produced -- the *shifted* one while
Shift is down, so Shift+3 reports `#` -- and the XKB keycode of the key in `nativeScanCode`. The
unshifted character is therefore the keysym the *keymap* has for that keycode at shift level 0 of the
active group, which `XkbKeymap::levelZeroKeysymOf()` answers. `KeysymTable.hpp` turns it into a
codepoint, and is also what reads a keysym when no keymap is reachable.

### What was investigated

Qt 6.11 headers (`QtGui/qguiapplication_platform.h`), the native interfaces of `QGuiApplication`:

| Interface | Exposes |
|-----------|---------|
| `QNativeInterface::QX11Application` | `Display*`, `xcb_connection_t*` |
| `QNativeInterface::QWaylandApplication` | `wl_display`, `wl_seat`, `wl_keyboard`, ... and `xkb_context*` |

Nothing in Qt's public API exposes an `xkb_keymap` or `xkb_state`. Qt keeps them in its private
`QXkbCommon` support and its platform plugins.

### Decision

**X11: libxkbcommon-x11 over Qt's own xcb connection.** `xkb_x11_keymap_new_from_device()` fetches the
keymap the X server is actually serving from the connection `QX11Application::connection()` hands out,
so it is the keymap the keycodes in the events belong to -- including layouts set by the desktop
environment, which the process environment knows nothing of. The keymap is fetched once and is the only
state: no `xkb_state` is made, and nothing is asked of the server per key.

**The group comes from the event.** The X11 core key event state carries the active XKB group (layout
index) in bits 13 and 14, and Qt's xcb plugin passes that state through unchanged as
`QKeyEvent::nativeModifiers()` (`QXcbKeyboard::handleKeyEvent` hands `e->state` to the `QKeyEvent`).
`XkbKeyboardPlatform` decodes it (`xcbGroupOf()`), puts it in `NativeKeyCode::group`, and the layout asks
`XkbKeymap::levelZeroKeysymOf(keycode, group)`, which looks up
`xkb_keymap_key_get_syms_by_level(keymap, keycode, group, 0)` with the group clamped to the last one the
key has (`xkb_keymap_num_layouts_for_key`). A lookup therefore allocates nothing and never blocks the GUI
thread, and a switch between configured layouts is followed because the next event says so. An earlier
design built an `xkb_state` from the device per lookup; that was a heap allocation and a synchronous X
round trip for every key event, releases included.

**Known limitation: a rewrite of the keymap on the server is not followed.** The keymap is fetched once. If
the keymap itself changes while Contour runs -- `setxkbmap`, a desktop environment applying new layouts, a
keyboard hotplug that reloads it -- keycodes keep resolving against the old keymap until Contour is
restarted, so unshifted keys can be wrong in the meantime. The follow-up is to refresh the keymap on the XKB
extension's `NewKeyboardNotify` / core `MappingNotify` events; Qt consumes those on its own connection
today, so that needs an event filter (`QAbstractNativeEventFilter`) on the xcb connection.

**Wayland: no live keymap; the keysym table alone.** The compositor sends its keymap to Qt as a file
descriptor on `wl_keyboard`, which Qt's plugin consumes privately. `QWaylandApplication` exposes only
the `xkb_context`, which is a factory and holds no keymap. Wayland therefore falls back to reading the
event's own keysym through the table: right for every key held without Shift, and for a shifted one only
where the shifted and unshifted characters coincide. This is a known gap, not a claim.

`makeLiveXkbKeymap()` returns `nullptr` under any Qt platform that is not xcb, and under no Qt
application at all, so offscreen runs and Wayland take the same fallback path.

### Deliberate Windows behaviour: the layout's letter is the key

On Windows the key a press reports as its unshifted key (the kitty protocol's key code) is what the
active layout types for the virtual key, through `ToUnicodeEx`. On a Russian layout the `A` key therefore
reports `ф` (1092), where the US table gave `a`. This is intended -- the protocol names a key by what it
types -- and the characterization row "win32: Cyrillic letter on a Russian layout under kitty report-all" pins it
with a fixed Russian layout, since the live call exists on Windows only.

### Rejected alternatives

1. **Qt's native interface for the keymap** -- candidate (1): does not exist (above).
2. **`xkb_keymap_new_from_names()` with the user's RMLVO from the environment** -- candidate (3), kept
   only as the seam the tests drive (`makeXkbKeymapFromNames()`). As a *source* it is wrong more often
   than the fallback: `XKB_DEFAULT_*` are honoured by only some compositors, desktop environments set
   the layout through their own settings, and an unset environment compiles `us` -- so a German user
   would see `-` where the key types `ß`, worse than the table's honest "I do not know". It cannot see
   the active group either.
3. **Hooking `wl_keyboard` ourselves** to capture the keymap fd: a second listener on Qt's proxy,
   racing Qt's own, through private surfaces. Rejected as fragile for a Wayland-only gain.
4. **Qt private headers** (`QXkbCommon`): not installed with Qt development packages by every
   distribution, and unstable across minor releases.
5. **Reading `_XKB_RULES_NAMES` from the X root window** and compiling that: the names a keymap was
   *built from*, not the keymap, and blind to the group.

### Dependencies

`contour_input` links `xkbcommon` and `xkbcommon-x11` through pkg-config on Linux and the BSDs
(`if(UNIX AND NOT APPLE)`); neither Windows nor macOS has XKB. The development packages are
`libxkbcommon-dev` and `libxkbcommon-x11-dev` (Debian/Ubuntu), `libxkbcommon-devel` and
`libxkbcommon-x11-devel` (Fedora), `libxkbcommon-x11-devel` on openSUSE. Each distribution's
`libxkbcommon-x11` development package depends on the core one and on `xcb`.

### Where it is built

`makePlatformKeyboardLayout()` asks for the keymap when the app's keyboard layout is first used
(`ContourGuiApp::keyboardLayout()`), not when the app object is constructed, because the xcb connection
exists only once the Qt application does.
