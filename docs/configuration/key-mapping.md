# Key Mapping
To customize key mappings, you need to configure `input_mapping` yaml entry

!!! warning "Defining `input_mapping` replaces the built-in bindings — it does not add to them"

    As soon as an `input_mapping` section is present in your `contour.yml`, Contour's built-in
    defaults (`Ctrl+Shift+C`/`Ctrl+Shift+V`, the `Ctrl+Shift+P` command palette, the tab and pane
    shortcuts, …) are discarded, and only the entries you list are active. Writing an
    `input_mapping` section is a declaration that you are defining the full set of bindings.

    The `contour.yml` that Contour generates on first run already contains every default binding
    written out in full, so editing *that* section keeps the defaults you did not touch. If you are
    writing a configuration by hand and want to keep the defaults, start from a generated one
    (`contour generate config`) and add your entries to the existing list.

## Built-in bindings

A small number of bindings are built in and do **not** appear in `input_mapping`:

| Binding | Action |
|---------|--------|
| `Ctrl+Tab` / `Ctrl+PageDown` | Switch to the tab on the right (wrapping around) |
| `Ctrl+Shift+Tab` / `Ctrl+PageUp` | Switch to the tab on the left (wrapping around) |
| `Ctrl+Shift+G` | Open the last command's output in the pager named by `command_blocks.pager` (needs a shell that marks its commands with [OSC 133](../vt-extensions/osc-133-shell-integration.md)). With nothing to open the chord is swallowed rather than sent to the shell as `^G`; on the alternate screen it goes to the application |
| `Alt+Shift+Arrow` (Linux, Windows) / `Cmd+Option+Arrow` (macOS) | Move the focus to the pane on that side. Some desktops and keyboard-layout switchers grab `Alt+Shift`; if the chord never reaches Contour, bind another one to `FocusPaneLeft`/`Right`/`Up`/`Down` in `input_mapping` |
| `Ctrl+Shift+E` / `Ctrl+Shift+O` (Linux, Windows) | Split the active pane side by side / stacked |
| `Cmd+D` / `Cmd+Shift+D` (macOS) | Split the active pane side by side / stacked |
| `Cmd+Shift+Enter` (macOS) | Zoom the active pane to fill the tab, and back |
| `Ctrl+Shift+K` (Linux, Windows) / `Cmd+Shift+K` (macOS) | `NewWindowTo`: open the [Connect picker](../remote-hosts.md#choosing-where-a-window-lives-the-connect-picker) to choose where a new window's sessions live |
| `Right` mouse button | Open the terminal's context menu |
| `Ctrl` + triple-click with the `Left` mouse button | Select the output of the command under the pointer (needs a shell that marks its commands with [OSC 133](../vt-extensions/osc-133-shell-integration.md); on the alternate screen, which has no commands, the click selects as usual) |
| Horizontal wheel / two-finger sideways swipe | Switch tabs |

These are consulted only when your `input_mapping` does not claim the same key or button (for the
triple-click, the same button and click count), so binding one of them yourself replaces it.

They are built in rather than listed as defaults for the reason given in the warning above: an
`input_mapping` section replaces the built-in list wholesale, and the generated `contour.yml` writes
every default into that section. A binding added to Contour *after* your configuration file was
written would therefore be shadowed by your own file forever. Consulting these after yours is what
lets a new binding still reach you, while anything you bind explicitly keeps winning.

!!! note "`Ctrl+Tab` is taken from the application"

    Like Windows Terminal, GNOME Terminal and Konsole, Contour claims `Ctrl+Tab` for tab switching,
    so a terminal application that wants it (via the Kitty keyboard protocol) will not receive it.
    Bind it in your own `input_mapping` to give it back to the application.

## macOS defaults

On macOS the application's chords are Command chords, as in any Mac application, and the default
bindings follow that:

* The Ctrl+Shift defaults answer to `⌘` instead (`⌘C`, `⌘V`, `⌘T`, `⌘W`, `⌘N`, `⌘K`, `⌘,`, `⇧⌘P`, …), and
  `Ctrl+Shift+letter` reaches the program in the terminal. A `⌘` chord nothing is bound to is consumed rather
  than typed as its letter.
* Font size is on `⌘0`, `⌘-` and `⌘+` only; `Ctrl+0` reaches the program.
* `⌘X` and `⌘S` are deliberately unbound (a terminal cannot cut, and there is nothing to save), so
  `ClosePane` and `SpeakSelection` have no macOS default; bind them if you want them.
* The defaults that hung on `Alt` plus a character are not installed on macOS, because Option composes
  characters there (see below). Tabs stay on `⌘1` to `⌘9`.
* `⌥←` / `⌥→` move by word (`ESC b` / `ESC f`) outside the alternate screen, and `⌘←` / `⌘→` / `⌘⌫` edit the
  line, so these shell conventions work out of the box. Inside the alternate screen `⌥←` reaches the program as the Alt+Left it is (`CSI 1;3D`).

### Option as Alt

Option has two jobs on a Mac: composing characters (`⌥E` then `E` gives é) and acting as Alt. The profile
setting `option_as_alt` chooses which Option keys are Alt: `off`, `left`, `right` or `both` (`true` and
`false` are still read as `both` and `off`). One rule applies everywhere:

* An Option key that `option_as_alt` names is Alt, for every key: `⌥B` sends `ESC b`, and an `Alt+B`
  binding fires.
* An Option key it does not name composes on any key that has a composed character (`⌥E`, `⌥1` gives ¡) and
  is Alt only on keys that type nothing (arrows, Return, Backspace, the function keys), so `⌥←` and
  `Option+Enter` keep working with `option_as_alt: off`.

Because of the second half, a binding of `Alt` plus a character cannot fire on an Option key that composes.
Contour says so when it loads such a binding with `option_as_alt` set to `off` or to one side; bind the
chord to `Primary` or `Control` instead, or name that side in `option_as_alt`.

## Writing bindings

Each element in the `input_mapping` represents one key binding, whereas `mods` represents an array of keyboard modifiers that must be pressed - as well as the `key` or `mouse` - in order to activate the corresponding action. 
Additionally one can filter input mappings based on special terminal modes using the `modes` option:

* Alt       : The terminal is currently in alternate screen buffer, otherwise it is in primary screen buffer.
* AppCursor : The application key cursor mode is enabled (otherwise it's normal cursor mode).
*  AppKeypad : The application keypad mode is enabled (otherwise it's the numeric keypad mode).
* Select    : The terminal has currently an active grid cell selection (such as selected text).
* Insert    : The Insert input mode is active, that is the default and one way to test that the input mode is not in normal mode or any of the visual select modes.
* Search    : A search term is currently set. It outlives the find bar, so this stays true for as long as the matches are still highlighted; an open bar with nothing typed into it does not match.
* Trace     : The terminal is currently in trace-mode, i.e., each VT sequence can be interactively single-step executed using custom actions. See TraceEnter/TraceStep/TraceLeave actions.

You can combine these modes by concatenating them via `|` and negate a single one by prefixing with `~`. The `modes` option defaults to not filter at all (the input mappings always match based on modifier and key press/mouse event).
`key` represents keys on your keyboard, and `mouse` represents buttons as well as the scroll wheel.

Modifiers, which are case-insensitive (`Control`, `control` and `CONTROL` are the
same modifier):

* `Shift`
* `Alt`
* `Control` — may also be written `Ctrl`
* `Super` — may also be written `Meta`. This is the Windows key on Windows, the Command key on
  macOS, and the Super key on anything else.
* `Hyper`

!!! note

    `Meta` is a synonym for `Super` for historical reasons: `contour.yml` has spelled the
    Windows/Command key `Meta` since before a separate `Meta` modifier existed. There is currently
    no configuration spelling for the distinct `Meta` modifier that the extended CSI u keyboard
    protocol reports.

* `Primary` — the chord an application's own commands hang on: Command (`Super`) on macOS,
  `Control` + `Shift` everywhere else. One row `mods: [Primary], key: C, action: CopySelection` is
  therefore `Ctrl+Shift+C` on Linux and Windows and `⌘C` on macOS, and Contour's built-in defaults are
  written that way. `Primary` can be combined with further modifiers (`[Primary, Shift]`), and the
  generated `contour.yml` spells it `Primary` wherever a default row uses it, so one file works on every
  machine you share it between.

An entry naming a modifier that is not in this list is rejected as a whole — the binding is dropped
rather than bound with a partial chord — and the reason is reported on startup.

Keys can be expressed case-insensitively symbolic:

`APOSTROPHE, ADD, BACKSLASH, COMMA, DECIMAL, DIVIDE, EQUAL, LEFT_BRACKET,
   MINUS, MULTIPLY, PERIOD, RIGHT_BRACKET, SEMICOLON, SLASH, SUBTRACT, SPACE
   Enter, Backspace, Tab, Escape, F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
   DownArrow, LeftArrow, RightArrow, UpArrow, Insert, Delete, Home, End, PageUp, PageDown,
   Numpad_Divide, Numpad_Multiply, Numpad_Subtract, Numpad_Add, Numpad_Decimal, Numpad_Enter, Numpad_Equal,
   Numpad_0, Numpad_1, Numpad_2, Numpad_3, Numpad_4,
   Numpad_5, Numpad_6, Numpad_7, Numpad_8, Numpad_9`

or in case of standard characters, just the character.

A single-character `key` also matches case-insensitively: `key: 'q'` and `key: 'Q'` bind the same
chord. Shift is carried by `mods`, not by the letter's case, so writing an uppercase letter does
*not* mean "with Shift" — use `mods: [Shift]` for that.

 Mouse buttons can be one of the following self-explanatory ones:

   `Left, Middle, Right, WheelUp, WheelDown, WheelLeft, WheelRight`

A mouse binding may also name a click count with `clicks` (1 to 4). Such a binding fires on that
click of a multi-click sequence only, and it is consulted *before* Contour's own double-, triple- and
quadruple-click selection, so it replaces that selection for the chord it names:

```yaml
input_mapping:
    - { mods: [Control], mouse: Left, clicks: 3, action: SelectCommandBlock, target: pointer, part: output }
```

A binding without `clicks` fires on every press that no `clicks` binding claimed. Like the selection
it replaces, a `clicks` binding never takes a click that an application asked for, unless the
[`bypass_mouse_protocol_modifier`](index.md#bypass_mouse_protocol_modifiers) is held.

`clicks` applies to the `Left`, `Middle` and `Right` buttons only: a wheel step is never more than a
first click. An entry that gives it to a key or a wheel, or a count other than 1 to 4, is dropped and
the reason is reported on startup.


