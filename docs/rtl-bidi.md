# Right-to-left and bidirectional text

Contour lays out Hebrew, Arabic and other right-to-left scripts using the Unicode Bidirectional
Algorithm (UAX#9), and implements the control sequences of the
[terminal-wg BiDi recommendation](https://terminal-wg.pages.freedesktop.org/bidi/).

## The model, in one paragraph

**Bidirectional support is display-only.** The grid stores characters in *logical* order — the order
they arrived in — and that is the order escape sequences address, the order the cursor moves through,
and the order text lands on the clipboard. Reordering happens on the way to the screen, and mouse
positions undergo the inverse transform on the way back. So a selection that looks discontiguous on
screen can be perfectly contiguous in memory, and copying it gives you what you would expect.

**A paragraph, not a row, is the unit.** A paragraph runs from one hard newline to the next, so a
line that soft-wraps continues the paragraph above it. This is why reordering does not change when
you resize the window: the algorithm never sees a row in isolation.

**The cursor sits over a character, not between two.** It follows its character wherever the
reordering puts it. Only which edge of the cell it draws on changes — left in a left-to-right run,
right in a right-to-left one.

## Control sequences

| Sequence | Name | Meaning | Default |
|---|---|---|---|
| `CSI 8 h` | BDSM, implicit | The terminal reorders bidirectional text itself | **set** |
| `CSI 8 l` | BDSM, explicit | The application has already reordered; the terminal draws what it is given | |
| `CSI 1 SP k` | SCP | Character path: left-to-right | |
| `CSI 2 SP k` | SCP | Character path: right-to-left | |
| `CSI 0 SP k` | SCP | Restore the terminal's own default | **default** |
| `CSI ? 2500 h` / `l` | | Mirror box-drawing glyphs in a right-to-left context | reset |
| `CSI ? 2501 h` / `l` | | Autodetect each paragraph's direction from its first strong character | reset |
| `CSI ? 1243 h` / `l` | | Swap Left/Right arrow keys inside a right-to-left paragraph | **set** |

DEC's own modes are honoured too: `CSI ? 34 h` (DECRLM) selects a right-to-left page and is kept in
step with SCP, `CSI ? 36 h` (DECHEM) and `CSI ? 96 h` (DECRLCM) are recognised and reported.

Note that **2500, 2501 and 1243 are provisional**. The recommendation is a draft and says its numbers
are not final; they may change if it settles on others.

### Which direction wins

Autodetection is the more specific request, so it takes precedence: `CSI ? 2501 h` says *derive the
direction from the text*, whereas SCP and DECRLM only state a default to use when nothing else
applies. With autodetection reset, the order is SCP, then DECRLM, then left-to-right.

`CSI 0 SP k` exists so that an application can undo its own choice without having to know which way
the terminal's default points. Prefer it to sending `CSI 1 SP k`.

## What to expect on screen

```
printf 'abc שלום 123 def\n'
```

renders as `abc 123 םולש def`. The Hebrew reverses, and the digits — which follow it logically —
are drawn *before* it, because European digits inside a right-to-left run resolve to an even
embedding level. That is correct, and is the single most surprising thing about bidirectional text.

Brackets in a right-to-left run are drawn mirrored, so `(שלום)` still reads as parentheses around a
word rather than closing before it opens.

## Fonts

Arabic needs a font with real `arab` GSUB tables, or its letters render as unjoined isolated forms —
readable, but wrong, and a font that merely *covers* Arabic will fail this silently. Hebrew is less
demanding but still benefits from proper coverage.

On Fedora, **FreeMono** (`gnu-freefont`) is the one monospace face covering Hebrew *and* Arabic with
both `arab` and `hebr` GSUB:

```
contour display.font.regular="FreeMono"
```

`google-noto-naskh-arabic-fonts` and `google-noto-sans-hebrew-fonts` are excellent per-script
alternatives. Note that DejaVu Sans Mono, despite its reputation for coverage, has **no Hebrew** —
using it for Hebrew silently measures font fallback rather than the shaping path.

## Trying it

`examples/bidi-demo.cpp` prints every case above next to the rendering a conforming terminal
produces, so the two can be compared directly:

```
./out/clang-asan/examples/bidi-demo
```

Its reference renderings are wrapped in `U+202D LEFT-TO-RIGHT OVERRIDE` so that they display
verbatim rather than being reordered themselves.

## Implementation

The algorithm lives in libunicode (`unicode::bidi_resolve`), which passes the full UAX#9 conformance
suites — `BidiTest.txt` and `BidiCharacterTest.txt` — so Contour does not carry its own copy of the
rules. `src/vtbackend/Bidi.{h,cpp}` turns resolved levels into a per-line column permutation;
`RenderBufferBuilder` applies it; `text_shaper` shapes each run in its own direction.

A line containing no codepoint at or above U+0590 takes a fast path that allocates nothing, so
ordinary left-to-right output pays essentially no cost.
