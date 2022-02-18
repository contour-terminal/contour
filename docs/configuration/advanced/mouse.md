# Advanced Mouse configuration

This keyboard modifier can be used to bypass the terminal's mouse protocol,
which can be used to select screen content even if the an application
mouse protocol has been activated (Default: Shift).

The same modifier values apply as with input modifiers (see below).

    bypass_mouse_protocol_modifier: Shift

Modifier to be pressed in order to initiate block-selection
using the left mouse button.

This is usually the Control modifier, but on OS/X that is not possible,
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

