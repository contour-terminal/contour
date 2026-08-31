# Buffer Capture

The screen's text buffer can be captured via VT sequence suitable for shell integration, such as `fzf`.

## Request Syntax

```
CSI > Pl ; Pr , t
```

The `,` intermediate is required. The bare `CSI > Pl ; Pr t` this sequence originally used is xterm's
XTSMTITLE opcode — a genuine collision, which XTSMTITLE now owns.

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

## Permission

Capturing the buffer is guarded by the `permissions.capture_buffer` profile setting (`allow`, `deny`
or `ask`). A request that is refused — by configuration, by the user, or because it was addressed to a
background pane that has no window to ask in — is answered with the terminating empty `<data>` chunk
and nothing else. A refusal is therefore indistinguishable from capturing an empty screen, which is
deliberate: the sequence has no way to report "no reply is coming", so a client that reads until the
terminator is never left waiting.
