# Key Mapping
To customize key mappings, you need to configure `input_mapping` yaml entry
Each element in the `input_mapping` represents one key binding, whereas `mods` represents an array of keyboard modifiers that must be pressed - as well as the `key` or `mouse` - in order to activate the corresponding action. 
Additionally one can filter input mappings based on special terminal modes using the `modes` option:

* Alt       : The terminal is currently in alternate screen buffer, otherwise it is in primary screen buffer.
* AppCursor : The application key cursor mode is enabled (otherwise it's normal cursor mode).
*  AppKeypad : The application keypad mode is enabled (otherwise it's the numeric keypad mode).
* Select    : The terminal has currently an active grid cell selection (such as selected text).
* Insert    : The Insert input mode is active, that is the default and one way to test that the input mode is not in normal mode or any of the visual select modes.
* Search    : There is a search term currently being edited or already present.
* Trace     : The terminal is currently in trace-mode, i.e., each VT sequence can be interactively single-step executed using custom actions. See TraceEnter/TraceStep/TraceLeave actions.

You can combine these modes by concatenating them via `|` and negate a single one by prefixing with `~`. The `modes` option defaults to not filter at all (the input mappings always match based on modifier and key press/mouse event).
`key` represents keys on your keyboard, and `mouse` represents buttons as well as the scroll wheel.

Modifiers:

* Alt
* Control
* Shift
* Meta (this is the Windows key on Windows OS, ans the Command key on macOS, and Meta on anything else)<br />Keys can be expressed case-insensitively symbolic.

Keys can be expressed case-insensitively symbolic:

`APOSTROPHE, ADD, BACKSLASH, COMMA, DECIMAL, DIVIDE, EQUAL, LEFT_BRACKET,
   MINUS, MULTIPLY, PERIOD, RIGHT_BRACKET, SEMICOLON, SLASH, SUBTRACT, SPACE
   Enter, Backspace, Tab, Escape, F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
   DownArrow, LeftArrow, RightArrow, UpArrow, Insert, Delete, Home, End, PageUp, PageDown,
   Numpad_Divide, Numpad_Multiply, Numpad_Subtract, Numpad_Add, Numpad_Decimal, Numpad_Enter, Numpad_Equal,
   Numpad_0, Numpad_1, Numpad_2, Numpad_3, Numpad_4,
   Numpad_5, Numpad_6, Numpad_7, Numpad_8, Numpad_9`

or in case of standard characters, just the character.

 Mouse buttons can be one of the following self-explanatory ones:

   `Left, Middle, Right, WheelUp, WheelDown`


