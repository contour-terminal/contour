# Advanced Mouse configuration

This keyboard modifier can be used to bypass the terminal's mouse protocol,
which can be used to select screen content even if the an application
mouse protocol has been activated (Default: Shift).

The same modifier values apply as with input modifiers (see below).

    bypass_mouse_protocol_modifier: Shift

To keep mouse events from applications altogether (Mouse Forwarding), set
`mouse_forwarding` in a profile (Default: forwarded). With `blocked`, no
mouse report and no DEC locator event (DECELR) reaches the application, and
a locator position request (DECRQLP) is answered as from an unavailable
locator. Selecting, scrolling and following links work as if the application
had not asked, and on the alternate screen the wheel still becomes cursor keys
(alternate scroll, `?1007`). The application's request is still recorded, so
forwarding again resumes the protocol it asked for. A button's release is
reported exactly when its press was, so toggling mid-click leaves no button
stuck down.

DEC locator button reports also honour read-only mode, the application's own
keyboard lock (KAM), Vi modes other than Insert, and the bypass modifier, just
as mouse tracking reports do.

    profiles:
      main:
        mouse_forwarding: blocked

The `ToggleMouseForwarding` action flips this at runtime, from a key binding
or the command palette. It has no default binding. A profile switch or a
configuration reload re-imposes the profile's value.

    input_mapping:
      - { mods: [Control, Alt], key: 'M', action: ToggleMouseForwarding }

Modifier to be pressed in order to initiate block-selection
using the left mouse button.

This is usually the Control modifier, but on macOS that is not possible,
so Alt or Meta would be recommended instead.

Supported modifiers:

- Alt
- Control
- Shift
- Meta

Default: Control

    mouse_block_selection_modifier: Control

Selects an action to perform when a text selection has been made.

Possible values are:

|---------------------------|--------------------------------------------------
| None                      | Does nothing
| CopyToClipboard           | Copies the selection to the primary clipboard.
| CopyToSelectionClipboard  | Copies the selection to the selection clipboard. This is not supported on all platforms.

Default: CopyToSelectionClipboard

    on_mouse_select: SelectClipboard

