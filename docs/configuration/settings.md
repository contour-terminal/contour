# The Settings Window

Settings opens in a window of its own, one per application, from **Settings…** in the Contour menu
(<kbd>⌘</kbd>+<kbd>,</kbd> on macOS, <kbd>Ctrl</kbd>+<kbd>Shift</kbd>+<kbd>,</kbd> elsewhere), from the command
palette, or with the `OpenConfiguration` binding. Asking for it again brings the open window forward, and it
reopens on the page it was left on.

The sidebar lists five sections, then your profiles, then **Edit contour.yml…**, which opens the file in your
editor. A search field above them finds a row by its label, then its help text, then its key, in any case.

| Section           | What it holds                                                                                  |
| ----------------- | ---------------------------------------------------------------------------------------------- |
| General           | Windows, the command palette, and the advanced global options such as the rendering GPU.       |
| Appearance        | Window chrome and the tab bar.                                                                 |
| Keyboard          | Key bindings, as a list.                                                                       |
| Shell Integration | What shell integration draws: the gutter, command blocks and folding.                          |
| Profiles          | One page per profile: colors, window, text, scrolling, behaviour, cursor, bell, background, links, status line and more. |

A profile's page has a **live preview** at its top: a small terminal that shows the profile's font, size and
colors, and follows every change before it is written. **Colors**, **Links** and **Status Line** open as pages
of their own from the profile, and the Status Line fields are on both the profile's page and the Status Line page.

## Every change applies at once

There is no Apply or Save button. A change takes effect in every terminal that uses the profile, in every window,
and is written to disk within about 300 ms of the last edit. **Revert Changes**, at the top right, undoes
everything changed since the window opened, and is disabled until something has.

Settings writes only the files it owns: `settings.yml` for the global options, `profiles/<name>.yml` for each
profile it edits, and `colorschemes/<name>.yml` for each color scheme. It never writes `contour.yml`. Each write
replaces the whole file at once, so a crash never leaves half of one.

- **A file you edited meanwhile is never overwritten.** If an editor saves a profile while Settings has it
  open, Settings writes nothing, shows the version on disk, and says which file changed. That includes a file
  saved in a state that does not parse: it stays as you left it until you fix it.
- **Profiles and color schemes defined in `contour.yml` are read-only.** The page says "Defined in contour.yml"
  and offers **Duplicate to Edit**, which makes a copy Settings can change.
- **With `gui_config_locked: true`** every control is disabled and nothing is written; a banner says why.
- **A change that cannot be applied** (a value the loader would reject, or a folder you cannot write to) is refused
  with the reason, and the control goes on showing the value in effect.

Names for new profiles and color schemes follow the rules of every system the configuration may travel to, so
they cannot contain path separators or `..` and, because a configuration directory may be copied to Windows, may
not be one of its reserved device names such as `con`, `aux` or `nul`. A `profiles/aux.yml` or `nul.yml` you
already have on Linux still loads, can be edited and deleted.

## Colors

The Colors page picks the color scheme (one for both appearances, or one for light and one for dark) from a
gallery of every scheme with its eight ANSI colors. **New scheme** and **Duplicate to Edit** create a scheme;
a scheme made in Settings can be renamed and deleted. Renaming moves every profile that draws with it. A scheme
a profile defined in `contour.yml` draws with cannot be renamed or deleted here, and neither can one that is
still in use.

Every color well shows its contrast against the background and says whether it passes, so a scheme whose ink
does not read is found while you make it. A scheme file that does not parse is never renamed over: open it and
fix it first.

## Links and Status Line

The Links page edits `hint_patterns` with a preview line that shows what each pattern underlines, as
described in [Links](links.md). The Status Line page edits the indicator status line's segments with a preview
of the bar, as described in [the indicator status line](indicator-statusline.md).

## Option as Alt on macOS

`option_as_alt` takes `off`, `left`, `right` or `both`, so that one Option key can type accents (é, ü) while the
other acts as Alt. In Settings it is the **Use Option as Meta** row on the Keyboard group of a profile. `true`
and `false` in an existing file are still read, as `both` and `off`.
