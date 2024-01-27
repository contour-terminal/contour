# Line Reflow Reconfiguration

On resize, overly long lines, that would otherwise be cut off, are usually reflowed to the next line on modern terminals.

This extension allows toggling reflow for the current line and subsequent lines using a DEC mode (`2028`).

## Feature detection

Use `DECRQM` (`CSI ? 2028 $ p`) to detect support for line reflow reconfiguration.

## Using the feature

Use `CSI ? 2028 h` to enable text reflow on the current line and the following lines.

Use `CSI ? 2028 l` to disable text reflow on the current line and the following lines.

