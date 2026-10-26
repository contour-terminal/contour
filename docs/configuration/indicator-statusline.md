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
`{HistoryLineCount}` | number of lines in history (only available in primary screen)
`{Hyperlink}`        | reveals the hyperlink at the given mouse location
`{InputMode}`        | current input mode (e.g. INSERT, NORMAL, VISUAL)
`{ProtectedMode}`    | indicates protected mode, if currently enabled
`{SearchMode}`       | indicates search highlight mode, if currently active
`{SearchPrompt}`     | the active search pattern, if any (read-only; search is typed into the find bar)
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

`{Cwd}` and `{Context}` additionally take this attribute:

Parameter                 | Description
--------------------------|--------------------------------------------------------------------
`MaxWidth=N`              | the widest the item may draw, in columns; beyond it the middle is elided and both ends kept. Default `40` for `{Cwd}`, `32` for `{Context}`; a value that is not a number `> 0` keeps the default.

A role names a color by what it means rather than by its value, and takes it from the active color
scheme when the line is drawn, so it follows a scheme change: `accent` is palette color 4 (blue),
`success` 2 (green), `danger` 1 (red), `warning` 3 (yellow) and `muted` 8 (bright black). Roles are
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

