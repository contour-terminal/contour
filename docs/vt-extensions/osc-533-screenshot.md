# OSC 533 — Screenshot

Reads a rectangular region of the page back to the application: as text, as the VT sequences that
would reproduce it, or as the pixels it is actually rendered with.

This is the region-addressable counterpart to [buffer capture](buffer-capture.md). Buffer capture
answers "what scrolled past", counted in lines from the bottom of the page and reaching back into
scrollback; OSC 533 answers "what is on screen *here*", named as a rectangle and returned in a
chosen format.

!!! warning "Reading the screen is guarded"

    A screenshot hands whatever is on the page to whichever program asked — including one running
    over SSH, or one that is not the program you are typing at. It is therefore subject to the same
    permission wall as buffer capture, configured per profile under
    `profiles.<name>.permissions.screenshot` and defaulting to `ask`.

## Request Syntax

```
OSC 533 ; Pid ; Pt ; Pl ; Pb ; Pr ; Pf ST
```

Every parameter is optional and may be left empty; a bare `OSC 533 ST` asks for the whole main page
as plain text.

| Parameter | Meaning                                             | Default        |
|-----------|-----------------------------------------------------|----------------|
| `Pid`     | Opaque request id, echoed in every reply            | `0`            |
| `Pt`      | Top row of the region                               | `1`            |
| `Pl`      | Left column of the region                           | `1`            |
| `Pb`      | Bottom row of the region                            | last row       |
| `Pr`      | Right column of the region                          | last column    |
| `Pf`      | Format (see below)                                  | `0`            |

The coordinates are **one-based and inclusive on all four edges**, as DEC's rectangular-area
functions (DECCRA, DECFRA, DECERA, DECRQCRA) name them. A coordinate that is omitted, empty **or
zero** takes its default; one naming a cell beyond the page names the page's edge instead.

Unlike those DEC functions, the region is measured from the page's top-left corner and **not** from
the origin, so origin mode (DECOM) does not move it. A screenshot names the screen; whatever margins
the application happens to have set are not part of the question being asked.

Only the current buffer's main page is readable. Scrollback is not addressable here — use
[buffer capture](buffer-capture.md) for that.

### Formats

| `Pf` | Format        | Status      | Produced by |
|------|---------------|-------------|-------------|
| `0`  | Plain text    | Implemented | the grid    |
| `1`  | VT sequences  | Implemented | the grid    |
| `2`  | Sixel         | Reserved    | the renderer |
| `3`  | PNG           | Implemented | the renderer |
| `4`  | RGBA          | Implemented | the renderer |

**Plain text** is UTF-8, one LF-terminated line per row of the region, blank cells rendered as
spaces and a wide character contributing a single codepoint.

**VT sequences** is the same text plus the SGR sequences needed to reproduce its colors, each row
terminated by CRLF — this format exists to be written back to a terminal, where a bare LF would
leave every row starting where the one above it ended.

**PNG** is a whole PNG file — signature, `IHDR` and all — of the region as rendered.

**RGBA** is tightly-packed 8-bit-per-channel pixels with straight (not premultiplied) alpha, rows
top to bottom, exactly `Pw * Ph * 4` bytes. It exists so an application that is going to blit the
pixels anyway does not have to carry a PNG decoder; the reply's `Pw`/`Ph` are what give the flat run
its shape.

**Sixel** is **reserved, not absent**: the number is spoken for, but nothing here encodes one yet.
It has a number so that an application asking for it is told `UnsupportedFormat` rather than met
with silence, and so that implementing it later adds a row to a table rather than changing this
grammar.

### What the pixel formats need

The two pixel formats are produced by the **renderer**, not by the terminal engine, and that has
consequences an application should expect:

- **A session without a renderer cannot serve them.** A headless session has no font loaded and no
  GPU attached, so there are no rasterized glyphs to photograph. Such a request is answered
  `Unavailable` (`Ps = 6`) — which is a statement about the *session*, not about the protocol, so an
  application that gets it may usefully ask again once a window exists.
- **They photograph what is on screen, not what is in the grid.** The capture is a crop out of a
  real rendered frame, so it carries the cursor, any selection highlight, the background image and
  the current viewport — including a scrollback position, if the user has scrolled away from the
  bottom.
- **The reply is not prompt.** The frame must be submitted before its texture can be read back, so
  the capture completes a frame or two after the request. `Pid` is what matches the answer to the
  question.
- **They are large.** A 640×384 region is under 30 KB as PNG but very nearly a megabyte as RGBA,
  which base64 inflates by a further third. It arrives as several hundred chunks; see below.

## Response Syntax

```
PM 533 ; Pid ; Ps ; Pt ; Pl ; Pb ; Pr ; Pf ; Pw ; Ph ; <base64> ST   (Ps = 1)
PM 533 ; Pid ; Ps ST                                                  (Ps ≠ 1)
```

That is: `ESC ^ 533 ; … ESC \`.

The reply is a **PM (Privacy Message), not an OSC**, matching what buffer capture does with
`PM 314`. The request and the reply therefore never share a grammar: a captured reply replayed into
a terminal — through `script(1)`, a log file, `asciinema`, a multiplexer passthrough — cannot be
read as a fresh request, so there is no way to build a request/response loop out of one.

The geometry echoed back is the region **as resolved**, in the same one-based units the request
used, so a reply can be read without knowing what the terminal defaulted the request to.

`Pw` and `Ph` are the payload's extent **in pixels**, and are `0` for a format that has no pixel
extent — which keeps the payload at one fixed position whatever the format was. They are not
redundant for a pixel format: RGBA is a flat run of bytes and says nothing about its own shape, and
even PNG's region cannot be measured from its extent in cells without knowing the cell size and how
the crop was clipped to the rendered frame. Kitty's graphics protocol makes the same call, requiring
`s=`/`v=` alongside raw pixels.

### Status codes

| `Ps` | Meaning                                                    |
|------|------------------------------------------------------------|
| `0`  | End of screenshot — no further messages belong to this one  |
| `1`  | Data chunk                                                 |
| `2`  | Denied by the user or by configuration                     |
| `3`  | The request could not be read                              |
| `4`  | The format named is reserved, or is not a format           |
| `5`  | The region names no cell (its corners are inverted)        |
| `6`  | The format is implemented, but this session cannot produce it right now |

`4` and `6` are deliberately different answers. `UnsupportedFormat` says the *protocol* does not
offer what was asked for and never will in this version; `Unavailable` says this *session* cannot —
no renderer is attached, or the capture failed — and is worth retrying.

**Every request is answered**, including one that is refused. An application that writes a request
and then blocks on a read is never left waiting for a reply that is not coming — which matters
particularly because the permission wall can hold a request for as long as the user takes to answer
a dialog.

`Pid` is echoed in every reply message, including refusals, so a delayed answer can still be matched
to the request that asked for it. This follows DECRQCRA, whose `Pid` works the same way.

### Payload encoding and chunking

The payload is **base64**. This is not decoration. Screen content is arbitrary, the VT-sequence
format contains `ESC` by construction, and a raw `ST` anywhere in the payload would terminate the
reply early and leave the remainder of the screenshot to be read as *input* by whatever asked for
it — turning screen content into an escape-sequence injection against the requesting program.
Encoding also makes the reply 8-bit clean and its length predictable.

A screenshot larger than 4095 bytes is split across several `Ps = 1` messages, each carrying the
same header. A `Ps = 0` message always terminates the sequence, including when the screenshot is
empty — so a reader knows it has the whole thing without counting bytes.

The chunk size is a multiple of three, so every chunk but the last encodes to a whole number of
base64 quanta and only the last can carry padding. Both ways of reassembling therefore work:
decode each chunk and concatenate the results, or concatenate the encoded payloads and decode once.

## Examples

Capture the whole page as plain text:

```sh
printf '\033]533\033\\'
# PM 533 ; 0 ; 1 ; 1 ; 1 ; 24 ; 80 ; 0 ; 0 ; 0 ; <base64> ST
# PM 533 ; 0 ; 0 ST
```

Capture rows 1–5, columns 1–20, as VT sequences, tagging the request `42`:

```sh
printf '\033]533;42;1;1;5;20;1\033\\'
# PM 533 ; 42 ; 1 ; 1 ; 1 ; 5 ; 20 ; 1 ; 0 ; 0 ; <base64> ST
# PM 533 ; 42 ; 0 ST
```

Capture rows 1–10 as PNG, tagging the request `7`:

```sh
printf '\033]533;7;1;;10;;3\033\\'
# PM 533 ; 7 ; 1 ; 1 ; 1 ; 10 ; 80 ; 3 ; 640 ; 160 ; <base64> ST
# ... more chunks ...
# PM 533 ; 7 ; 0 ST
```

Ask a headless session for the same thing, and be told it cannot:

```sh
printf '\033]533;7;;;;;3\033\\'
# PM 533 ; 7 ; 6 ST
```

Ask for sixel, which has a number but no encoder behind it:

```sh
printf '\033]533;9;;;;;2\033\\'
# PM 533 ; 9 ; 4 ST
```

## Notes on interoperability

There is no registry for OSC numbers, and terminals do occasionally pick the same number for
different purposes. At the time of writing 533 is unallocated — the nearest neighbours in
[terminfo.dev's list](https://terminfo.dev/osc) are 440 (audio) and 555 (screen flash).
