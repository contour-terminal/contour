A terminal emulator's text stack.
=================================

TL;DR
-----

This document describes how rendering text is architectually implemented
in Contour, an in-early-development virtual terminal emulator, in order to
support complex unicode as well as (and especially) complex colored emoji.

Introduction
------------

Text rendering in a virtual terminal emulator can be as simple as just iterating over each
grid cell's character, mapping it to a Font's bitmap glyph, and rendering it to the target surface
at the appropriate position. But it can also be as complex as a web browser's text stack[1] if one
may want to do it right.

In contrast to web browsers (or word processors), terminal emulators are still rendering text the
way they did render text 50 years ago - plus some non-standard extensions that did arise over the decades
with regards to formatting.

Also, terminal screens weren't made with Unicode in mind, Unicode did not even exist back then, so
there were a few workarounds and non-standardized ideas implemented in order to display complex
unicode text and symbols in terminals without a common formal ground that
terminal application developers can rely on.

Text rendering in a terminal emulator puts some additional constraints on how to render, mostly
because character placement is decided before text shaping and must align to a fixed-size grid, which
makes it almost impossible to properly render traditional japanese text into the terminal, or
hewbrew right-to-left text (though, there is a handful of virtual terminal emulators that specialize
on the latter and an informal agreement on how to deal with wide characters on the terminal screen).

Not every character, or to be displayed symbol (such as emoji) is as wide as exactly one grid cell's
width, so additional measurements have to be taken into account for dealing with these characters as
well.

Unicode - a very quick rundown in the context of terminals
----------------------------------------------------------

Unicode is aiming to have one huge universal space where every imaginable "user perceived character"
can be represented. A "user perceived character" is what the user that looks at that character
thinks of as one unit. This is in direct contrast to what a character is in computer science. A
"user perceived character" can be as simple as one single codepoint (32-bit value representing that
character) and as complex as an ordered sequence of 7 unbreakable codepoints to compose one
single "user perceived character".

This places additional requirements to a virtual terminal emulator where each
grid cell SHOULD contain exactly one *"user perceived character"*
(also known as *grapheme cluster*), that is, an unbreakable codepoint sequence
of one or more codepoints that must not be broken up into multiple grid cells
before the actual text shaping or screen rendering has been performed.

Also, some grapheme clusters take up more than one grid cell in terms of
display width, such as Emoji usually take two grid cells in width in order
to merely match the Unicode (TR 51, section 2.2) specification's wording
that the best practice to display emoji is to render them in a square block.

Rendering text - a top level view
---------------------------------

A terminal emulator's screen is divided into fixed width and
(not necessarily equal) fixed height grid cells.
When rendering this grid, it is sufficient to iterate over each line and
column and render each grid cell individually,
at least when doing basic rendering.

Now, when non-trivial user perceived characters need to be supported,
the rendering cannot just render each character individually, but must be first
grouped into smaller chunks of text with common shared properties, across
the grid cell boundaries.

Here we enter the world text shaping.

Text Shaping
------------

Simply put, text shaping is the process of translating a sequence of codepoints
into glyphs and their glyph positions. This differs from normal text processors
and web browsers in a way because glyph placement in virtual terminal emulators
are constrained.

When shaping text of a single grid line, the line is split into words,
delimited by spaces, gaps and SGR attributes, that is, each word must represent
the same SGR attributes for every "user perceived character"
(for example text style or background color must be equal for each position
in this sequence, from now on called "word").

The word can be used as a cacheable unit, in order to significantly speed up
rendering for future renders.
The cache key is composed of the codepoint sequence of that word, as well as,
the common shared SGR attributes.

This cacheable word is further segmented into sub runs by a series categories,
that is, by Unicode script attribute (such as Latin or Hangul)
and the symbol presentation (such as text presentation or Emoji presentation).
This is important because one cannot just pass a string of text to the
underlying text shaping engine with mixed properties, such as Hewbrew
text along with some latin and Kanji or Emoji in between or a font style change
for obvious reasons.
Each segment (usually called run) must be shaped individually with its own set
of fallback fonts. Emoji are using a different font and font fallback
list than regular text which uses a different font and font falback list then
bold, italic, or bold itaic fonts.
Emoji also have two different presentation styles, the one
that everybody expects and is named Emoji emoji presentation
(double-width colored emoji)
and the other one is named emoji text presentation, which renders emoji
in (usually narrow-width) monochrome pictogram.

The result of each sub run of a word is composing the sequence of glyph
and glyph positions that can be used as the cache value of a cacheable word.
The result of all sub runs can be used as the cache value for a cacheable word
to be passed to the next stage, the text renderer.

Text Rendering
--------------

The text renderer receives an already pre-shaped string of glyphs and
glyph positions relative to screen coordinates of the first glyph
to be rendered onto the screen.

In order to lower the pressure on the GPU and reduce synchronization times
between CPU and GPU, all glyph bitmaps are stored into a texture atlas
on the GPU, such that the text rendering (when everything has been already
uploaded once) just needs to deal with indices to those glyph bitmaps into
the appropriate texture atlas as well as screen coordinates
where to render those glyphs on the target surface.

There is one texture atlas for monochrome glyphs (this is usually standard text)
as well as one texture atlas for colored glyphs (usually colored emojis).
Additionally there can be a third type of texture atlas for storing
LCD anti-aliased bitmap glyphs.

Now, when rendering a string of glyphs and glyph positions, each glyph's
texture atlas ID and atlas texture coordinate is appended into an
atlas coordinate array along with each glyph's absolute
screen coordinate and color into a vertex buffer to be uploaded to the GPU.

When iterating over the whole screen buffer has finished,
the atlas texture and vertex buffer are filled with all glyphs and related
information that are reuiqred for rendering one frame.
These buffers are then uploaded to the GPU to be drawn in a single
GPU render command (such as `glDrawArrays`, or `glDrawElements`).

Other Terminal Emulator related Challenges
------------------------------------------

Most terminal applications use wcwidth() to detect the width of a potential "wide character". A
terminal emulator has to deal with such broken client applications. Some however use utf8proc's
`utf8proc_charwidth`, another library to deal with unicode.

The suggested way for future applications (emulator and client) would be
to introduce feature detection and mode switching on how to process
grapheme clusters and their width, if legacy apps are of concern.

Algorithmic wise, implementing grapheme cluster segmentation isn't too hard
but in execution very expensive. Also grapheme cluster width compuation
is expensive. But luckily, in the context of terminal emulators,
both can be optimized for the general case in terminal emulatoors, which is
mostly US-ASCII, and yields almost no penalty with optimizations or a
~60% performance penalty when naively implemented.

Also, implementing proper text shaping into a fixed-grid terminal wasn't really
the easiest when there is no other project or text to look at.
I used "Blink's text stack" documentation as the basis and mapped this
to the terminal world. Since text shaping *IS* expensive, this cannot be done
without caching without hurting user experience.

After investigating into the above optimization possibilities however, I do
not see why a terminal emulator should *not* do provide support for
complex unicode, as the performance I have achieved so far is above average at
least, and therefore should be suffient for everyday use.

Bidirectional text was not addressed in this document nor in the implementation
in the Contour terminal yet, as this imposes a new set of challenges
that have to be dealt with separately. Hopefully this will be eventually
added (or contributed) and this document will then be updated accordingly.

Conclusion
----------

If one went through all the pain on how Unicode, text segmentation, and
text shaping works, you will be rewarded with a terminal emulator that
is capable of rendering complex unicode. At least as much as most of us
desire - being able to use (power user/) programming ligatures and
composed Emoji.

Some terminal emulators do partly support ligatures or rendering trivial
single codepoint Emoji or a few of the composed Emoji codepoint sequences.
While this is a great start, I'd say we can deliver more.

Final notes
-----------

I'd like to see the whole virtual terminal emulator world to join forces
and agree on how to properly deal with complex text in a somewhat
future-proof way.

And while we would be in such an ideal world, we could even throw away all the
other legacies that are inevitably inherited from the ancient VT
standards that are partly even older than I am.
What would we be without dreams. ;-)


References
----------

- [Blink's text stack](https://chromium.googlesource.com/chromium/src/+/master/third\_party/blink/renderer/platform/fonts/README.md)
- [UTS 11](https://unicode.org/reports/tr11/) - character width
- [UTS 24](https://unicode.org/reports/tr24/) - script property
- [UTS 29](https://unicode.org/reports/tr29/) - text segmentation (grapheme cluster, word boundary)
- [UTS 51](https://unicode.org/reports/tr51/) - Emoji

