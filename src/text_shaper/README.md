# `text_shaper`

A maybe platform independent text shaping and rendering API
that can maybe used by other projects too.

### Font loading

Either via `shaper::load_font(font_description, font_size) -> font_key`,
or as part of font fallback, an alternate font at the same given size.

Loaded fonts are stored in a map associated with a `font_key`.

### Requirements

- libunicode
- for Linux:
  - freetype
  - harfbuzz
  - fontconfig
