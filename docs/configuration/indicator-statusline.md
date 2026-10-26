# Indicator Statusline

The indicator statusline used to be a feature, from the old DEC VT level 4 terminals.
Contour revives this feature to prominently show the terminal status.

## Configuration

```
profiles:
    your_profile:
        status_line:
            indicator:
                left: "{VTType} │ {InputMode:Bold,Color=#C0C030}{TraceMode:Bold,Color=#FFFF00,Left= │ }{ProtectedMode:Bold,Left= │ }"
                middle: "{Tabs}{Title:Left= « ,Right= » ,Color=#20c0c0}"
                right: "{HistoryLineCount:Faint,Color=#c0c0c0} │ {Clock:Bold} "
```

Each segment, `left`, `middle`, and `right` may contain text to be displayed in the
left, middle, or right segment of the indicator statusline.

This text may contain placeholders to be replaced by their respective dynamic content.

## Variables

Variable             | Description
---------------------|--------------------------------------------------------------------
`{Clock}`            | current clock in HH:MM format
`{Cwd}`              | the working directory the shell reports, with the home directory as `~` when it is on this machine; at most `MaxWidth=` columns (default `40`), eliding the middle directories (`/…/d59`)
`{Command}`          | yields the output of the given command, as specified via parameter `Program=...` (or the `{Command:<command>}` shorthand), re-run every `Interval=...` seconds
`{Extension}`        | what the [status-line extension](../extensions.md) named by parameter `Name=...` (or the `{Extension:<name>}` shorthand) draws
`{Git}`              | the branch of the Git repository the shell's directory is in (a ref outside `refs/heads/` as `remotes/origin/x`, a detached HEAD as its first 7 hex digits in parentheses, `(a1b2c3d)`), read from `.git/HEAD` or a worktree's `.git` file; empty outside a repository or in a directory on another machine. It is looked up in the background, so it shows a moment after the directory does, and again only after a command ends or the shell reports its directory, never per frame. As git does by default, the search for the repository stops at a filesystem boundary. The directory is walked as the shell reports it: in a symlink to a directory inside a repository, no branch shows A repository using the reftable ref backend (`git init --ref-format=reftable`) shows no branch for now
`{HistoryLineCount}` | number of lines in history (only available in primary screen)
`{Hyperlink}`        | reveals the hyperlink at the given mouse location
`{LastCommand}`      | how the last command went, from the shell integration's command blocks (OSC 133): `✓ make · 4ms` after a success, `✗ exit 8 ctest · 12s` after a failure; the status drawn in the `success` or `danger` role and the duration in `muted`, unless the item names a `Color=` of its own. Empty while a command runs, and when a newer prompt ended one that reported no exit status. A multi-line command's line breaks and tabs read as spaces. The command text is at most `MaxWidth=` columns (default `32`), cut at its end with `…`
`{InputMode}`        | current input mode (e.g. INSERT, NORMAL, VISUAL)
`{ProtectedMode}`    | indicates protected mode, if currently enabled
`{Scroll}`           | how far the view is scrolled back into the history, out of how far it can be: `↑ 1 240 / 10 000`, thousands grouped by a space. Empty at the bottom, and while the view cannot scroll (the alternate screen)
`{SearchMode}`       | indicates search highlight mode, if currently active
`{SearchPrompt}`     | the active search pattern, if any (read-only; search is typed into the find bar)
`{Selection}`        | how much the selection holds, as `y` would copy it: `12 lines · 418 chars`, thousands grouped by a space (`1 240`). Empty without a selection. The characters (cells that are not blank) are counted only while the selection spans fewer than 10 000 cells at the page's width; a larger one shows its lines alone, `1 240 lines`
`{Tabs}`             | indicates active tabs
`{Text}`             | given text (makes only sense when customized with flags)
`{Title}`            | current window title
`{VTType}`           | currently active VT emulation type

## Formatting Styles

Each Variable, as specified above, can be parametrized for customizing the look of it.
The common syntax to these variables and their parameters looks as follows:

```
{VariableName:SomeFlag,SomeKey=SomeValue}
```

So parameters can be specified after a colon (`:`) as a comma separated list of flags and key/value pairs.
A key/value pair is further split by equal sign (`=`).

The following list of formatting styles are supported:

Parameter                 | Description
--------------------------|--------------------------------------------------------------------
`Left=TEXT`               | text to show on the left side, if the variable is to be shown
`Right=TEXT`              | text to show on the right side, if the variable is to be shown
`Color=COLOR`             | text color: `#RRGGBB`, or one of the roles `accent`, `success`, `danger`, `warning`, `muted`
`BackgroundColor=COLOR`   | background color: `#RRGGBB`, or one of the roles `accent`, `success`, `danger`, `warning`, `muted`
`Bold`                    | text in bold font face
`Italic`                  | text in italic font face
`Underline`               | underline text (only one underline style can be active)
`CurlyUnderline`          | curly underline text (only one underline style can be active)
`DoubleUnderline`         | double underline text (only one underline style can be active)
`DottedUnderline`         | dotted underline text (only one underline style can be active)
`DashedUnderline`         | dashed underline text (only one underline style can be active)
`Blinking`                | blinking text
`RapidBlinking`           | rapid blinking text
`Overline`                | overline text
`Inverse`                 | inversed text/background coloring

These parameters apply to all variables above.

`{Cwd}`, `{Context}` and `{LastCommand}` additionally take this attribute:

Parameter                 | Description
--------------------------|--------------------------------------------------------------------
`MaxWidth=N`              | the widest the item may draw, in columns; beyond it the middle is elided and both ends kept. Default `40` for `{Cwd}`, `32` for `{Context}` and for `{LastCommand}`'s command text, which is cut at its end instead; a value that is not a number `> 0` keeps the default.

A role names a color by what it means rather than by its value, and takes it from the active color
scheme when the line is drawn, so it follows a scheme change: `accent` is palette color 4 (blue),
`success` 2 (green), `danger` 1 (red), `warning` 3 (yellow) and `muted` 8 (bright black). The scheme
is the configured one: a program changing the palette (OSC 4) recolours its own text, not the status
line. Where a role's color would not read on the bar it is drawn on (at 4.5:1), the role's text takes
the bar's legible ink instead -- the scheme's foreground or background, bright white, then black or
white -- so it stays readable in every input mode and scheme; a `#RRGGBB` color is drawn as written. Roles are
read in any case and written lowercase. A value that is neither a role nor a color leaves the color
unset. The same values are accepted by the `Tabs` attributes `ActiveColor` and `ActiveBackground`.

The `Command` variable additionally takes these attributes:

Parameter                 | Description
--------------------------|--------------------------------------------------------------------
`Program`                 | the command to execute (required, unless the shorthand below is used)
`Interval`                | seconds between runs; default `10`. An invalid value or one `<= 0` selects the default.

```
{Command:Program=git rev-parse --abbrev-ref HEAD,Interval=30,Bold}
```

**Shorthand.** `{Command:<command>}` takes everything after `Command:` verbatim as the command, so it
may contain commas and `=`, but no styling and no `Interval=`: `{Command:git log -1 --format=%h,%s}`.
A template naming `Program=` or `Interval=` is read in the attribute form instead; anything else
is the shorthand, so `{Command:Bold}` runs a program named `Bold` (a style flag needs `Program=`). A command cannot contain `}`.

**Execution.** Commands run asynchronously in the background, so they never slow down rendering
and, on Windows, never open a console window. The item shows the output of the previous run until a
new one arrives, and is empty before the first run completes. A command that is still running after
5 seconds (or after its `Interval`, if that is shorter) is killed together with its child processes,
and the previous output is kept.

Only the first line of the command's standard output is shown. Standard error and the exit status are
ignored, and a command that cannot be started shows the operating system's error message.

Unlike `popen`, background processes a command leaves behind are killed as soon as the command
finishes, even when it succeeds (the process group on Unix, the job object on Windows): a command like
`start notepad` starts nothing that outlives it.

### Tabs formatting extensions

The `Tabs` key allows additional styling through the following attributes:

Parameter                 | Description
--------------------------|--------------------------------------------------------------------
`ActiveColor`             | color of the active tab
`ActiveBackground`        | background color of the active tab

!!! note "`{Tabs}` versus the GUI tab bar"

    The indicator status line belongs to a *pane*, so with split panes a window draws one `{Tabs}`
    list per pane rather than one per window — several tab lists side by side, each describing the
    same tabs.

    If what you want is the terminal look rather than the placement, set the global
    [`ui_style`](index.md#ui_style) to `terminal`. That keeps the single window-level tab bar and
    every one of its affordances (drag to reorder, tear off, rename, close, context menu) but draws
    it cell-quantized, in the terminal font, with square corners and box-drawing separators.

    `{Tabs}` remains fully supported, and the two can be used together.

