# Grid (re)design

## grid properties

- line and column positions are given in offsets (0-based indices!)
- grid cells of the full scrollback + main page area are stored in dense linear space
- `Line` is just a view into the ring buffer
- logical lines above main page area's line count cannot be edited
- scroll offset represents the *bottom* line of the viewport.
- maybe make scroll offset negative (to have all ops being additions)

## corollary

- the VT screen line/column position to offset translation (minus 1)
  happens as early as possible in the `Sequencer`.
- `Line::Wrapped`-flag can be removed.
- writing an overlong line does not need to care about auto-wrapping
  - because `lineLength / pageWidth == numberOfLinesWriten`
  - the new cursor position can be computed analogous to the above
- because of the meaning of the scroll-offset, reflow can be O(1) implemented
  with a `Line()` being arbitrary long(!) and the top screen line being
  computed by subtracting `(PageLineCount - 1) * PageColumnCount)`,
  then *just* lineary walking forward until the bottom right.
  - this enables relative jumps (`CUU` etc) to jump up logical lines.
  - we could make the cursor jump behavior configurable, via DEC mode.
- On char write overflow
  - if Reflow DEC mode is enabled, then continue appending character
  - else if AutoWrap DEC mode is enabled, then linefeed()
  - else overwrite character on right margin
- I think with the above approach we do not need DEC mode 2027 for disabling
  reflow in order to protect command prompts. as they're automatically protected
  by the grid line architecture.

### Notes

Suppose only a single overly long line is written and ocupies the full screen
(many page counts).

```
bottomRightOffset = line(cursor.line + realOffset(scrollOffset_)).right_offset;
topLeftOffset = bottomRightOffset - pageSize.lines * pageSize.columns;

def renderViewport():
{
    for (auto i = topLeftOffset, k = 0; i < bottomRightOffset; ++i, ++k)
    {
        auto rowNr = k / pageSize.columns;
        auto colNr = k % pageSize.columns;
        renderCell(buffer_[i], rowNr, colNr);
    }
}
```

## shrink columns

    page size: 6x2
    lines: 2
    line.1: 0..5        abcdef
    line.2: 6..11       ABCDEF

    shrink to pageSize: 4x2

    line.1:  0..3       abcd
    line.2:  4..5       ef      Wrapped
    line.3:  6..9       ABCD
    line.4: 10..11      EF      Wrapped

Shrinking does not destroy, as it always reflows.
`CUU` will move by logical line numbers.

## overlong lines

4x2


    line.1 ABCDEFabcdef
    line.2 ABCD

    view:

        history.line.-1 ABCD
        history.line.0  EFAB
        logical.line.1  cdef
        logical.line.2  ABCD

## Rendering a wrapped line

- the scroll-offset represents the number of real lines scrolled up
-
