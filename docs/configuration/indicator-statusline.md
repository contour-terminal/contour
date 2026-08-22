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
`{Command}`          | yields the result of the given command prompt, as specified via parameter `Program=...`
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
`Color=#RRGGBB`           | text color in hexadecimal RGB notation
`BackgroundColor=#RRGGBB` | background color in hexadecimal RGB notation
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

The `Command` variable is the only one that requires a special attribute, `Program` whose value
is the command to execute.

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

