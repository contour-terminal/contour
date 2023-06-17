# Buffer Capture

The screen's text buffer can be captured via VT sequence suitable for shell integration, such as `fzf`.

## Request Syntax

```
CSI > Pl ; Pr t
```

`Pl` is  `1` if the lines are to be counted logically and `0` if the lines are to be counted visually.

A logical line is may be a wrapped line that spans more than one visual line, whereas a visual line
always maps to exactly one line on the screen.

The parameter `Pr` is the number of lines to be captured.

## Response Syntax

```
PM 314 ; <data> ST
```

The response is may span multiple `PM` sequences.
The reply will always end with a PM message with an empty `<data>` block, denoting the end of the reply.

Each `<data>` chunk will be UTF-8 encoded of the text lines to be captured. Each line will be delimited
by a newline escape sequenced (`LF`).
