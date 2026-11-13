# Key Mapping
To customize key mappings, you need to configure `input_mapping` yaml entry

!!! warning "Defining `input_mapping` replaces the built-in bindings — it does not add to them"

    As soon as an `input_mapping` section is present in your `contour.yml`, Contour's built-in
    defaults (`Ctrl+Shift+V`, the `Ctrl+Shift+P` command palette, the tab shortcuts, …) are discarded,
    and only the entries you list are active. Writing an
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
| `Ctrl+Shift+Alt+PageDown` / `Ctrl+Shift+Alt+PageUp` (Linux, Windows) / `Ctrl+Cmd+Down` / `Ctrl+Cmd+Up` (macOS) | Show the next / previous workspace of the window's daemon (`CycleWorkspace`, wrapping around) |
| `Ctrl+Shift+G` (Linux, Windows) / `Cmd+Shift+O` (macOS) | Open the last command's output in the pager named by `command_blocks.pager` (needs a shell that marks its commands with [OSC 133](../vt-extensions/osc-133-shell-integration.md)). With nothing to open the chord is swallowed rather than sent to the shell as `^G`; on the alternate screen it goes to the application |
| `Ctrl+Shift+L` (Linux, Windows) / `Cmd+K` (macOS) | Clear the scrollback and the screen above the current prompt (`ClearToPrompt`). On the primary screen only; with nothing to clear the chord is swallowed rather than typed |
| `Alt+Shift+Arrow` (Linux, Windows) / `Cmd+Option+Arrow` (macOS) | Move the focus to the pane on that side. Some desktops and keyboard-layout switchers grab `Alt+Shift`; if the chord never reaches Contour, bind another one to `FocusPaneLeft`/`Right`/`Up`/`Down` in `input_mapping` |
| `Ctrl+Shift+E` / `Ctrl+Shift+O` (Linux, Windows) | Split the active pane side by side / stacked |
| `Cmd+D` / `Cmd+Shift+D` (macOS) | Split the active pane side by side / stacked |
| `Cmd+Shift+Enter` (macOS) | Zoom the active pane to fill the tab, and back |
| `Ctrl+Shift+K` (Linux, Windows) / `Cmd+Shift+K` (macOS) | `NewWindowTo`: open the [Connect picker](../remote-hosts.md#choosing-where-a-window-lives-the-connect-picker) to choose where a new window's sessions live |
| `Right` mouse button | Open the terminal's context menu |
| `Ctrl` + triple-click with the `Left` mouse button | Select the output of the command under the pointer (needs a shell that marks its commands with [OSC 133](../vt-extensions/osc-133-shell-integration.md); on the alternate screen, which has no commands, the click selects as usual) |
| Horizontal wheel / two-finger sideways swipe | Switch tabs |

The default chords added since your configuration was generated (see [Default chords](#default-chords)) reach
it the same way, among them `Ctrl+Shift+C`, `Ctrl+Shift+W`, the pane chords and the macOS `⌘P`, `⌘W` and `⌃⌘`
chords.

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

## Default chords

Every command with a default chord has one on Linux and Windows **and** on macOS. On macOS the
application's chords are Command chords: a `Ctrl+Shift+letter` default answers to `⌘letter` there
(`Primary`, see below). Where macOS already gives the plain `⌘` chord another meaning, the Mac chord adds
Shift or moves, and the table says why.

| Command | Linux / Windows | macOS | Note |
|---------|-----------------|-------|------|
| Command palette (`OpenCommandPalette`) | `Ctrl+Shift+P` | `⌘P`, `⇧⌘P` | `⇧⌘P` for VS Code, Zed and Ghostty habits |
| Daemon and workspace switcher (`ShowSwitcher`) | `Ctrl+Shift+R` | `⇧⌘R` | |
| Copy (`CopySelection`) | `Ctrl+Shift+C`; `Ctrl+C` while selecting | `⌘C` | |
| Paste (`PasteClipboard`) | `Ctrl+Shift+V` | `⌘V` | |
| Paste without control characters | `Ctrl+Alt+V` | `⌃⌘V` | Option composes characters on a Mac, so the `Ctrl+Alt` chords become `⌃⌘` |
| New window (`NewTerminal`) | `Ctrl+Shift+N` | `⌘N` | |
| New window to… (`NewWindowTo`) | `Ctrl+Shift+K` | `⇧⌘K` | `⌘K` clears to the prompt |
| New tab (`CreateNewTab`) | `Ctrl+Shift+T` | `⌘T` | |
| Close pane (`ClosePane`) | `Ctrl+Shift+W`, `Ctrl+Shift+X` | `⌘W` | Closes the tab with its last pane |
| Close tab (`CloseTab`) | — | `⇧⌘W` | On Linux and Windows, `Ctrl+Shift` already holds Shift |
| Select tab 1…9 (`SwitchToTab`) | `Alt+1` … `Alt+9`, `Alt+0` for the tenth | `⌘1` … `⌘9` | `⌘0` resets the font size |
| Previous / next tab | `Ctrl+PageUp` / `Ctrl+PageDown`, `Ctrl+Shift+Tab` / `Ctrl+Tab`, `Shift+←` / `Shift+→` | the same, and `⇧⌘[` / `⇧⌘]` | |
| Move tab left / right | `Ctrl+Shift+PageUp` / `PageDown` | `⌘PageUp` / `⌘PageDown` | |
| Previous / next workspace (`CycleWorkspace`) | `Ctrl+Shift+Alt+PageUp` / `Ctrl+Shift+Alt+PageDown` | `⌃⌘↑` / `⌃⌘↓` | `Ctrl+Alt+PageUp`/`PageDown` stay Emacs's `C-M-prior`/`C-M-next` |
| Split side by side / stacked | `Ctrl+Shift+E` / `Ctrl+Shift+O` | `⌘D` / `⇧⌘D` | iTerm2's chords |
| Focus the pane on that side | `Alt+Shift+Arrow` | `⌥⌘Arrow` | |
| Swap with the pane on that side | `Ctrl+Alt+Arrow` | `⌃⌥Arrow` | |
| Resize the pane | `Ctrl+Shift+Alt+Arrow` | `⌃⇧⌥Arrow` | |
| Zoom the pane (`TogglePaneZoom`) | `Ctrl+Shift+Z` | `⇧⌘↩` | iTerm2's chord |
| Toggle split orientation | `Ctrl+Shift+\` | `⌃⇧\` | |
| Show or hide the workspace sidebar (`ToggleSidebar`) | `Ctrl+Shift+B` | `⌘\` | `native2026` only |
| Jump to the next pane that needs you (`JumpToNextAttention`) | `Ctrl+Shift+J` | `⌘J` | `native2026` only |
| Focus the workspace sidebar (`FocusSidebar`) | `Ctrl+Shift+D` | `⇧⌘E` | `native2026` only |
| Find (`SearchReverse`) | `Ctrl+Shift+F` | `⌘F` | |
| Next / previous match | `F3` / `Shift+F3` | `⌘G` / `⇧⌘G`, and `F3` / `Shift+F3` | |
| Find the selection (`SearchSelection`) | — | `⌘E` | A macOS convention; `Ctrl+Shift+E` splits |
| Clear search highlight | `Ctrl+Shift+H` | `⇧⌘H` | `⌘H` hides the application |
| Previous / next command (`FocusPreviousCommand`) | `Ctrl+↑` / `Ctrl+↓` | `⌘↑` / `⌘↓` | At the prompt only, as in VS Code's terminal |
| Select previous / next command's output | `Ctrl+Shift+↑` / `Ctrl+Shift+↓` | `⇧⌘↑` / `⇧⌘↓` | At the prompt only |
| Previous / next prompt (`ScrollMarkUp`) | `Ctrl+Alt+K` / `Ctrl+Alt+J` | `⌃⇧↑` / `⌃⇧↓` | At the prompt only |
| Open the last command's output | `Ctrl+Shift+G` | `⇧⌘O` | `⌘G` finds the next match |
| Clear to the prompt (`ClearToPrompt`) | `Ctrl+Shift+L` | `⌘K` | The shell's `Ctrl+L`, plus Shift |
| Open URLs by hint (`HintMode`) | `Ctrl+Shift+U` | `⌘U` | |
| Font size larger / smaller / reset | `Ctrl+Shift+=` / `Ctrl+Shift+-` / `Ctrl+0` | `⌘+` / `⌘-` / `⌘0` | |
| Toggle the status line | `Ctrl+Alt+.` | `⌃⌘.` | |
| Open the file manager | `Ctrl+Alt+O` | `⌃⌘O` | |
| Screenshot (`ScreenshotVT`) | `Ctrl+Alt+S` | `⌃⌘S` | |
| Toggle input method handling | `Ctrl+Alt+,` | `⌃⌘,` | |
| Settings (`OpenConfiguration`) | `Ctrl+Shift+,` | `⌘,` (menu bar), `⌃⇧,` | |
| Vi normal mode | `Ctrl+Shift+Space` | `⌃⇧Space` | |
| Full screen | `Alt+Enter` | `⌥↩` | |
| Speak the selection | `Ctrl+Shift+S` | — | macOS has its own Speak Selection (`⌥Esc`) |
| Quit | `Ctrl+Shift+Q` | `⌘Q` (menu bar) | |

A test (`ChordParity_test.cpp`) keeps the two columns complete: a command bound on one platform only fails
the build unless it is listed there with its reason.

## macOS defaults

* The menu bar provides `⌘Q` (Quit) and `⌘,` (Settings…), and `⌘H`, `⌥⌘H` and `⌘M`; Contour binds none of them by
  default, so they never reach `input_mapping`. `Ctrl+Shift+letter` reaches the program in the terminal, and a
  `⌘` chord nothing is bound to is consumed rather than typed as its letter.
* `⌘K` clears to the prompt (`ClearToPrompt`) on the primary screen; in the alternate screen it is consumed, so a
  running vim or tmux is never reset. `ClearHistoryAndReset` has no macOS default.
* Font size is on `⌘0`, `⌘-` and `⌘+` only; `Ctrl+0` reaches the program.
* `⌘X` and `⌘S` are deliberately unbound (a terminal cannot cut, and there is nothing to save).
* The defaults that hang on `Ctrl+Alt` plus a character answer to `⌃⌘` on macOS instead, because Option
  composes characters there (see below).
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
Additionally one can filter input mappings based on special terminal modes using the `mode` option:

* Alt       : The terminal is currently in alternate screen buffer, otherwise it is in primary screen buffer.
* AppCursor : The application key cursor mode is enabled (otherwise it's normal cursor mode).
*  AppKeypad : The application keypad mode is enabled (otherwise it's the numeric keypad mode).
* Select    : The terminal has currently an active grid cell selection (such as selected text).
* Insert    : The Insert input mode is active, that is the default and one way to test that the input mode is not in normal mode or any of the visual select modes.
* Search    : A search term is currently set. It outlives the find bar, so this stays true for as long as the matches are still highlighted; an open bar with nothing typed into it does not match.
* Trace     : The terminal is currently in trace-mode, i.e., each VT sequence can be interactively single-step executed using custom actions. See TraceEnter/TraceStep/TraceLeave actions.

Every condition separated by `|` must match. Prefix a condition with `~` to require that mode to be inactive. For example, `Insert|Select` requires both Insert mode and a terminal selection. Omit `mode` to match regardless of the active modes.

To make Ctrl+V paste from the clipboard in Insert mode, including at a prompt with no selection, use the following entry under `input_mapping`.

```yaml
- { mods: [Control], key: V, action: PasteClipboard, mode: Insert }
```

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

## Switching daemons and workspaces

These actions re-point the window they fire in at another workspace or daemon (see
[Switching a window](../persistent-sessions.md#switching-a-window)). Only `CycleWorkspace` is bound by default:
`Ctrl+Shift+Alt+PageDown` / `PageUp` (Linux, Windows) and `⌃⌘↓` / `⌃⌘↑` (macOS) show the next / previous
workspace, beside the tab chords one level down. The command palette lists them all under **Daemons**. They need a window that shows a daemon.

| Action | Key | What it does |
| --- | --- | --- |
| `SwitchToWorkspace` | `index:` (1-based, required) | Shows the Nth workspace of this window's daemon here. |
| `NewWorkspace` | none | Creates a workspace and shows it in this window; for a window attached to a daemon, the workspace is created on that daemon. |
| `OpenWorkspace` | `index:` (1-based, required) | Shows it in a new window. |
| `CycleWorkspace` | `direction: next\|previous` (default `next`) | Shows the next or previous workspace of this window's daemon here, hidden and left ones included, in their position order. |
| `CycleDaemon` | `direction: next\|previous` (default `next`) | Shows the next or previous daemon here: the local daemons running, by label, then the saved hosts a window here shows. |
| `SwitchToDaemon` | `label:` or `host:` (exactly one) | Shows the named local daemon, or saved host, here. |
| `OpenDaemon` | `label:` or `host:` (exactly one) | Shows it in a new window. |

```yaml
input_mapping:
  - { mods: [Control, Alt], key: '2', action: SwitchToWorkspace, index: 2 }
  - { mods: [Control, Shift, Alt], key: PageDown, action: CycleWorkspace, direction: next }
  - { mods: [Control, Alt, Shift], key: RightArrow, action: CycleDaemon, direction: next }
  - { mods: [Control, Alt], key: A, action: SwitchToDaemon, label: admin }
  - { mods: [Control, Alt], key: H, action: OpenDaemon, host: home }
```

The cycling actions never wait on a daemon: they step through what was last looked up, and look again in
the background for the next press, so the first press after a change (or right after start-up) may still
see the earlier list.

A binding that gives no `index` (or 0), an unknown `direction`, or neither or both of `label` and `host`
is dropped, and the reason is reported on startup. Landing on a daemon with several workspaces opens the
switcher popup on that daemon's workspaces, to pick one; `Esc` there cancels the switch and changes nothing.

| Action | What it does |
| --- | --- |
| `ShowSwitcher` | Opens the daemon and workspace switcher popup over this window. |
| `NewDaemon` | Asks for a label, starts a local daemon under it, and opens a new window on it. |
| `UpgradeDaemon` | Upgrades this window's local daemon in place, keeping every program running (palette: **Upgrade daemon**). |

The switcher popup lists the daemons under **This computer** (the local daemons running, by label) and
**Saved hosts**. This window's daemon is expanded; `Right` expands any other one to its workspaces, each with
its tab count and whether it is shown in *this window*, *shown elsewhere*, *hidden* or *not shown*, plus
"+ new workspace on …". A saved host not connected yet says so, and is connected to when it is expanded or
chosen. Typing filters the list, `Return` switches this window to the selected row, `Shift+Return` opens it
in a new window, and `Esc` closes the popup without changing anything.

`NewDaemon` (also the popup's **New daemon…** row) refuses a label that cannot name a socket (letters,
digits, `-`, `_` and `.` only, not starting with a dot, at most 64 characters) right under the field; a
label that is already running is offered as "Switch to daemon …" instead of starting a second one. The daemon
it starts keeps running when its last window closes, like one started with `contour daemon --label`.

`UpgradeDaemon` (the palette's **Upgrade daemon** row) starts a [live upgrade](../persistent-sessions.md#live-upgrade)
of the window's daemon at any time, without the daemon-drift chip: it also upgrades a daemon that runs the
same release, to pick up a rebuilt binary. It asks nothing, as an upgrade keeps every program running. It is
for the local daemon this client launched on (one started with **New daemon…** is not); for a remote daemon, one a switch reached, a daemon
too old to upgrade in place, or one newer than this client it does nothing but tell you why in a notice. It
is not bound to a key by default.

## Workspace sidebar

The [workspace sidebar](../sidebar.md) is shown and started by the `native2026` defaults (`defaults_edition`),
which also bind its chords. A configuration with `defaults_edition: classic` binds none of them. A `native2026`
configuration written before the sidebar existed still gets them, unless it binds the same chord itself.

| Action | Default (Linux / Windows) | Default (macOS) | What it does |
| --- | --- | --- | --- |
| `ToggleSidebar` | `Ctrl+Shift+B` | `⌘\` | Shows the sidebar when hidden, hides it when shown. |
| `FocusSidebar` | `Ctrl+Shift+D` | `⇧⌘E` | Moves keyboard focus into the sidebar, showing it first when hidden. |
| `FilterWorkspaces` | none | none | Opens the sidebar's filter with the keyboard in it, showing the sidebar first when hidden. |
| `JumpToNextAttention` | `Ctrl+Shift+J` | `⌘J` | Shows the next pane that needs you: blocked or bell first, then failed, then done and not seen. |
| `MarkSeen` | none | none | Marks the active pane's output as seen. |
| `RenameWorkspace` | none | none | Renames the current workspace inline. |
| `MoveWorkspaceUp`, `MoveWorkspaceDown` | none | none | Moves the current workspace one place in the sidebar's list. |
| `CloseWorkspace` | none | none | Closes the current workspace and everything in it, after asking. The last workspace of an in-process window cannot be closed. |
| `GoToPane` | none | none | Shows the pane a sidebar row names; a row of the pane menu. |
| `MoveTabToWorkspace` | `index:` (1-based, required) | | Moves the current tab into the Nth workspace. |

`NewWorkspace` is listed under [Switching daemons and workspaces](#switching-daemons-and-workspaces); the
sidebar's **＋** button runs it.

The settings are `sidebar: { visible, width }` and `tab_placement: sidebar | top`; see
[the sidebar guide](../sidebar.md#configuration).
