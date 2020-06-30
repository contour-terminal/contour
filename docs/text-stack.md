A terminal emulator's text stack.
=================================

TL;DR
-----

This document describes how rendering text is implemented in the Contour, an in-early-development
virtual terminal emulator, in order to support complex unicode as well as (and especially) colored emoji.

Introduction
------------

Text rendering in a virtual terminal emulator can be as simple as just iterating over each
grid cell's character, mapping it to a Font's bitmap glyph, and rendering it to the target surface
at the appropriate position.  But it can also be as complex as a web browser's text stack if one
may want to do it right. And yet, there are challenges that do not exist in the modern era of computer world.

In contrast to web browsers (or word processors), terminal emulators are still rendering text the
way they did render text 40 years ago - plus some non-standard extensions that did arise over the decades
with regards to formatting.

Also, terminal screens weren't made with Unicode in mind, Unicode did not even exist back then, so
there were numerous workarounds and non-standardized ideas implemented in order to display complex
unicode text and symbols in terminals.

Text rendering in a terminal emulator puts some additional constraints on how to render, mostly
because character placement is decided before shaping and must align to a fixed-size grid, which
makes it almost impossible to properly render traditional japanese text into the terminal, or
hewbrew right-to-left text (though, there is a handful of virtual terminal emulators that specialize
on the latter).

Not every character, or to be displayed symbol (such as emoji) is as wide as exactly one grid cell's
width, so additional measurements have to be taken into account for dealing with these characters as
well.

Unicode - a very quick rundown in the context of terminals
----------------------------------------------------------

Unicode is aiming to have one huge universal space where every imaginable "user perceived character"
can be represented. A "user perceived character" is what the user that looks at that character
thinks of as one unit. This is in direct contrast to what a character is in computer science. A
"user perceived character" can be as simple as one single codepoint (32-bit value representing that
character) and as complex as an ordered sequence of 18 unbreakable codepoints to compose one
single "user perceived character".

This places additional requirements to a virtual terminal emulator where each grid cell contains
exactly one "user perceived character", that is, an unbreakable codepoint sequence of more than one
codepoint must not be broken into multiple grid cells before the actual text shaping or screen
rendering has been performed.

Also, some "user perceived characters" (or symbols) take up more than one grid cell, such as Emoji
usually take two grid cells in width.

Rendering text - a top level view
---------------------------------

A terminal emulator's screen is divided into fixed width and fixed height grid cells.
When rendering this grid, it is sufficient to iterate over each line and column and render each grid
cell individually.
Now, when non-trivial user perceived characters need to be supported, the rendering cannot just
render each character individually, but must be first grouped into smaller chunks of text with
common shared properties.

Text Shaping
------------

Text shaping is the process of translating the string of codepoints into glyphs and glyph positions.
This differs from normal text processors and web browsers in a way because glyph placement in
virtual terminal emulators are constrained.

When shaping text of a single grid line, the line is split into words, delimited by spaces and SGR
attributes, that is, each word must represent the same SGR attributes for every "user perceived
character" (for example background color or text style must be equal for each position in this
word).

This word can be used as a cacheable unit, in order to speed up rendering for future render calls.
The cache key is composed of the codepoint sequence of that word as well as the common shared SGR
attributes.

This cacheable word is split into sub runs by categories, that is, by Unicode script attribute as
well as symbols with their presentation style. This is important because one cannot just pass a
string of text to the underlying text shaping engine with mixed script properties, such as Hewbrew
text, along with some latin and Kanji or Emoji in between. Each script segment must be shaped
individually with its own set of fallback fonts. Emojis are using a different font and font fallback
list than regular text, for example. Emojis also have two different presentation styles, the one
that everybody expects and is named emoji-presentation (colored emoji display, usually double-width)
and the other one is named emoji-text-presentation, which renders emoji in monochrome (usually narrow-width).

The result of each sub run of a word is composing the array of glyph and glyph positions that can be
used as the cache value of a cacheable word. The result of all sub runs can be used as the cache
value for a cacheable word to be passed to the next stage, the text renderer.

Text Rendering
--------------

The text renderer receives an already pre-shaped string of glyphs and glyph positions relative to
screen coordinates of the first glyph to be rendered onto the screen.

In order to lower the pressure on the GPU and reduce synchronization times between CPU and GPU, all
glyph bitmaps are stored into a 3D texture atlas on the GPU, such that the text rendering (when
everything has been already uploaded once) just needs to deal with indices to those glyphs
into the appropriate texture atlas as well as screen coordinates where to render those glyphs on the
target surface.

There is one 3D texture atlas for monochrome glyphs (this is usually standard text) as well as one
3D texture atlas for colored glyphs (usually colored emojis).

Now, when rendering a string of glyphs and glyph positions, each glyph's texture atlas ID and atlas
texture coordinate is appended into an atlas coordinate array along with each glyph's absolute
screen coordinate into a screen coordinate array.

When iterating over the whole screen buffer has finished, the atlas texture coordinate and screen
texture coordinate arrays are filled with all glyphs that must be rendered. These arrays are then
uploaded to the GPU to be drawn in a single GPU render command (such as glDrawArrays).

Other Terminal Emulator related Challenges
------------------------------------------

Most terminal applications use wcwidth() to detect the width of a potential "wide character". A
terminal emulator has to deal with such broken client applications. Some however use utf8proc's
`utf8proc_charwidth`, another library to deal with unicode.

Final notes
-----------

I'd like love to see the whole virtual terminal emulator world to join forces and agree on how to
properly deal with text in a future-proof way, and while we would be in such an ideal world, we
could even throw away all the other legacies that are inevitably inherit from the ancient VT
standards that are partly even older than I am. What would we be without dreams. ;-)

