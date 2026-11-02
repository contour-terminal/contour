# Keyboard input

How a key press becomes bytes for the program in the terminal: one native event, normalized by the
platform's `KeyboardPlatform`, routed against the bindings, then encoded. This page records the
decisions behind the parts that are not obvious from the code.

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
