# ReGIS Vector Graphics

Contour implements **ReGIS** (Remote Graphics Instruction Set), DEC's vector-graphics language
introduced with the VT330/VT340 terminals. Where [Sixel](images.md) transmits a raster (a grid of
pixels), ReGIS transmits *drawing instructions* — lines, circles, arcs, curves, filled polygons and
text — which the terminal rasterises. This makes ReGIS compact and resolution-independent: a circle
is a few bytes, not a bitmap.

This page is a practical reference for people who script and program terminals. It matches Contour's
implementation; where Contour deviates from the VT340 it is called out explicitly.

!!! note "Authoritative reference"

    The behaviour here follows the [VT330/VT340 Programmer Reference Manual, Volume 2: Graphics
    Programming](https://vt100.net/docs/vt3xx-gp/contents.html). That manual is the ground truth for
    ReGIS semantics.

## Entering and leaving ReGIS

ReGIS is carried inside a **DCS** (Device Control String):

```
ESC P p  <ReGIS commands>  ESC \
```

`ESC P` (0x1B 0x50) is the DCS introducer, the letter `p` selects ReGIS, and `ESC \` (String
Terminator) ends it. An optional numeric parameter before `p` controls state carry-over:

| Introducer | Meaning |
|------------|---------|
| `ESC P p` or `ESC P 0 p` | Resume: position, colours, write controls and the addressing window carry over from the previous ReGIS string. |
| `ESC P 1 p` / `ESC P 3 p` | Reset the graphics state and clear the canvas before drawing. |

From a shell you can emit ReGIS with `printf`:

```sh
# Draw a diagonal line from the top-left toward the middle of the screen.
printf '\033PpP[100,100]V[400,300]\033\\'
```

### Detecting ReGIS support

ReGIS-capable terminals advertise feature **3** in their primary Device Attributes reply. Send
`CSI c` (`ESC [ c`) and look for `;3` in the response:

```sh
printf '\033[c'    # Contour replies e.g. ESC [ ? 65 ; ... ; 3 ; 4 ; ... c   (3 = ReGIS, 4 = Sixel)
```

You can also query the ReGIS graphics geometry with XTSMGRAPHICS (`CSI ? 3 ; 1 S`); Contour reports
the fixed VT340 addressing area of `800 x 480`.

## The coordinate system

ReGIS addresses an **800 × 480** pixel screen. The origin `[0,0]` is the **top-left** corner and **Y
increases downward**, so `[799,479]` is the bottom-right. Contour rasterises the ReGIS canvas and
scales it across the terminal window, drawing it above the text so that painted pixels overlay the
cells and untouched areas remain transparent.

Coordinates are written in brackets, `[x,y]`. Each axis is independent and may be:

| Form | Meaning |
|------|---------|
| `[100,50]` | Absolute position. |
| `[+30,-10]` | Relative to the current position (signed). |
| `[100]` | X only; Y unchanged. |
| `[,50]` | Y only; X unchanged. |
| `[]` | The current position (used by `V` to plot a dot). |

### Pixel vectors

Outside brackets, a run of the digits **0–7** moves in one of eight compass directions, each step
scaled by the *pixel-vector multiplier* `W(M...)`:

```
    3   2   1
      \ | /
   4 -- + -- 0
      / | \
    5   6   7
```

`0` = right, `2` = up, `4` = left, `6` = down, and the odd digits are the diagonals. For example
`V0000` draws a line four pixels to the right.

## Commands

Every ReGIS command is a single, case-insensitive letter, optionally followed by parenthesised
*options* `( ... )`, bracketed *coordinates* `[ ... ]`, pixel-vector digits, and quoted strings. A
`;` ends a command. Whitespace is ignored between tokens.

### `P` — Position

Moves the graphics cursor without drawing.

```sh
printf '\033PpP[400,240]\033\\'     # move to the centre
```

- `[x,y]` or pixel-vector digits move the cursor.
- `(B)` / `(S)` begin a bounded / unbounded position stack, `(E)` ends it.
- `(W...)` applies temporary write controls (see `W`).

### `V` — Vector

Draws straight lines from the current position, honouring the current colour, line pattern and
width.

```sh
printf '\033PpP[100,100]V[400,100]V[400,300]V[100,300]V[100,100]\033\\'   # a rectangle
```

- `[x,y]` draws a line to that point and moves there.
- `[]` plots a single dot at the current position.
- Pixel-vector digits draw along each step.

### `C` — Curve

Circles, arcs and interpolated curves.

```sh
printf '\033PpP[400,240]C[500,240]\033\\'      # circle of radius 100, centred at the cursor
```

- `C[x,y]` — a **circle** whose centre is the current position and whose radius reaches `[x,y]`.
- `C(C)[x,y]` — the bracket is the **centre**; the current position lies on the circumference.
- `C(A<degrees>)[x,y]` — an **arc**; a positive angle sweeps counter-clockwise, negative clockwise.
- `C(S)[...]...(E)` — an **open** interpolated curve through the listed points.
- `C(B)[...]...(E)` — a **closed** interpolated curve (the cursor returns to the start).

### `W` — Write controls

Sets the state that governs all drawing. Options combine inside one `W( ... )`.

| Option | Effect |
|--------|--------|
| `I<n>` / `I(<spec>)` | Foreground colour: a register index, or a colour spec (see [Colour](#colour)). |
| `V` `R` `C` `E` | Writing mode: **V** overlay (default), **R** replace, **C** complement, **E** erase. |
| `P<n>` | Line pattern 0–9 (see [Patterns](#patterns)); `P(M<n>)` sets the pattern multiplier. |
| `M<n>` | Pixel-vector multiplier (pixels per PV step). |
| `L<n>` | Line width in pixels. |
| `N<0/1>` | Invert (negate) the line pattern. |
| `S...` | Shading (see [Shading](#shading)). |

When written inside another command as `(W...)`, the controls apply only to that command.

### `T` — Text

Draws text with anti-aliased glyphs. In the GUI, ReGIS text is shaped by the same font engine as
normal terminal text; the terminal engine itself ships a self-contained bitmap font as a fallback
(headless use, or before a font is configured). An injected interface keeps the terminal engine free
of any font-stack dependency.

```sh
printf "\033PpP[100,100]T(S2)'Hello, ReGIS'\033\\"
```

- `'text'` or `"text"` — the string to draw at the current position.
- `(S<0-16>)` — a standard cell size (0 = 9×10, up to 16 = 144×240); `(S[w,h])` a custom cell.
- `(M[wf,hf])` — width/height multipliers; `(H<n>)` a height multiplier.
- `(D<degrees>)` — writing direction in 45° steps (0 = rightward, 90 = up).
- `(I<degrees>)` — italic slant.

### `S` — Screen

Screen-wide operations.

| Option | Effect |
|--------|--------|
| `(E)` | Erase the canvas to the background. |
| `(A[ul][lr])` | Set the addressing window (defaults to `[0,0][799,479]`). |
| `(I<n>)` / `(I(<spec>))` | Background colour. |
| `(M<i>(<spec>)...)` | Program colour registers (the colour map). |

```sh
printf '\033PpS(E)S(A[0,0][799,479])\033\\'    # clear and reset the coordinate window
```

Page selection, scrolling, output scaling, hardcopy and time-delay sub-options are accepted but have
no effect in Contour (they describe hardware Contour does not emulate).

### `F` — Polygon fill

Fills the region enclosed by a set of vectors or a curve.

```sh
printf '\033PpP[300,100]F(V[500,100][400,280])\033\\'   # a filled triangle
```

- `F(V...)` fills the polygon traced by the vector operations (the current position is the first
  vertex).
- `F(C...)` fills a curved region.

### `R` — Report

Queries state and sends a reply to the host application.

| Request | Reply |
|---------|-------|
| `R(P)` | The graphics cursor position, as `[x,y]`. |
| `R(P(I))` | The interactive locator (mouse) position, as `<button>[x,y]`. Contour reports it immediately rather than blocking. |
| `R(E)` | The last error code (`0` = none). |
| `R(L)` | The current alphabet. |

### `@` — Macrographs

Stores and replays command strings.

```sh
# Define macro A as a box, then invoke it twice at different positions.
printf '\033Pp@:AV[+100,0]V[0,+100]V[-100,0]V[0,-100]@;P[100,100]@AP[300,100]@A\033\\'
```

- `@:X ... @;` defines macro `X`.
- `@X` invokes macro `X` (expanded recursively; Contour bounds the depth and output size).
- `@.` clears all macros.

### `L` — Load character set

Down-loadable custom character sets are recognised but not rendered in Contour (as in several other
ReGIS implementations); text uses the built-in font.

## Colour

Contour keeps 16 colour registers, initialised to the VT340 default palette. A colour is named in
three ways:

- **Register index** — `W(I5)` selects register 5.
- **Named colour** — `W(I(R))`, where `D`=dark, `R`=red, `G`=green, `B`=blue, `C`=cyan, `Y`=yellow,
  `M`=magenta, `W`=white.
- **HLS or RGB spec** — `W(I(H<0-360>L<0-100>S<0-100>))` for hue/lightness/saturation, or
  `W(I(R<0-100>G<0-100>B<0-100>))`. On the DEC hue wheel `0°` is blue, `120°` red, `180°` yellow,
  `240°` green.

Registers are reprogrammed with `S(M<index>(<spec>))`, e.g. `S(M1(H240L50S100))` makes register 1
green.

## Patterns

`W(P<n>)` selects one of ten standard line patterns:

| n | Pattern | n | Pattern |
|---|---------|---|---------|
| 0 | (none) | 5 | dash-dot-dot |
| 1 | solid (default) | 6 | sparse dot |
| 2 | dash | 7 | asymmetric sparse dot |
| 3 | dash-dot | 8 | sparse dash-dot |
| 4 | dot | 9 | sparse dot-dash |

A custom pattern is a bit string, e.g. `W(P11110000)`.

## Shading

Shading fills the area between a drawn primitive and a reference line:

- `W(S1)` turns shading on (reference defaults to the cursor's current Y); `W(S0)` turns it off.
- `W(S[,Y])` sets a horizontal reference at `Y`; `W(S(X)[X])` a vertical reference at `X`.

```sh
# Shade the area under a line down to y=300.
printf '\033PpW(S[,300])P[100,100]V[400,200]\033\\'
```

## Example programs

Contour ships four ReGIS demos under
[`examples/`](https://github.com/contour-terminal/contour/tree/master/examples):

- **`regis-primitives.cpp`** — a static showcase of every primitive (lines, circles, arcs, curves,
  fills, patterns, colours and text). A good visual conformance check.
- **`regis-plot2d.cpp`** — a labelled 2D plot of a mathematical function with axes and a grid.
- **`regis-animation.cpp`** — an animated analog clock, redrawing each frame with `S(E)` + redraw.
- **`regis-plot3d.cpp`** — an **interactive** 3D plot of the complex function `|z² − 1|`; drag with
  the mouse to rotate it (horizontal = yaw, vertical = pitch), press `q` to quit.

Build them with the test build enabled and run them inside Contour:

```sh
cmake --build --preset clang-release --target regis-primitives regis-plot3d
./out/clang-release/examples/regis-primitives
./out/clang-release/examples/regis-plot3d      # then drag with the mouse
```
