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

### macOS font lookup

macOS uses `CoreTextLocator`; Linux uses `FontconfigLocator`. Both feed the shared
FreeType/HarfBuzz `OpenShaper`, but the platform font lookup implementations are separate.

On macOS, explicitly configured fallback families are matched independently, in configuration
order. Require the family name when matching a descriptor, while allowing the nearest available
weight and slant. The traits dictionary belongs under `kCTFontTraitsAttribute`. Only an
unspecified fallback list uses Apple's default language-dependent cascade; that cascade is not
an inventory of user-installed fonts.

When fallback is enabled and the shared shaper exhausts a chain, `resolve` uses `CTFontCreateForString` for the missing
span. CoreFoundation transcodes the UTF-32 span; measure the resulting CoreText range in UTF-16
code units, preserving
supplementary characters and variation selectors. Reject LastResort rather than treating its
placeholder glyphs as character coverage. Coverage recovery honors disabled fallback and a zero
fallback limit. Strict spacing still governs the configured chain. CoreText's final
character-specific recovery permits proportional faces, as DirectWrite's recovery does: symbols
such as U+23F1 are absent from the Nerd Font files and can require a proportional system math
font even in a monospace terminal. Existing locators retain their strict coverage policy through
FontLocator::coverageFallbackSpacing; Linux behavior is unchanged. The cache includes the
resulting spacing eligibility, so answers cannot leak across different policies.

Preserve the selected face when translating CoreText fonts to FreeType paths: a font collection
URL can name several faces. Match the CoreText PostScript name against FreeType collection faces
and named variable-font instances, and retain the resulting face index. The shared shaper cache
and failed-source list must also include that index in font identity. The native locator maps
each file once and discovers collection names lazily, under a mutex protecting its FreeType
library and tables. It checks file identity, size, mtime and ctime before reuse, invalidating
replaced or updated files. Descriptors are queried afresh; the cache stores no DPI-dependent
metrics or character-coverage decisions. DPI changes continue to refresh the existing shaper
faces and metrics in place, preserving issued font keys. CoreFoundation objects
returned by Create/Copy APIs have one owner; local autorelease pools also cover background calls.

References:

- [Apple: matching font descriptors](https://developer.apple.com/documentation/coretext/ctfontdescriptorcreatematchingfontdescriptor(_:_:))
- [Apple: default cascade lists](https://developer.apple.com/documentation/coretext/ctfontcopydefaultcascadelistforlanguages(_:_:))
- [Apple: character-specific fallback](https://developer.apple.com/documentation/coretext/ctfontcreateforstring(_:_:_:))
- [FreeType: faces and collection indices](https://freetype.org/freetype2/docs/reference/ft2-face_creation.html#ft_facerec)

`CoreTextLocator_test.cpp` runs only on macOS and uses system fonts, so it requires no Nerd Font
installation. It covers explicit order, missing and duplicate families, disabled/default chains,
collection styles, concurrent lookups, Unicode edge cases, and shaping a supplementary emoji
after the configured chain is exhausted. DPI round trips preserve primary/fallback keys and
refresh metrics and bitmap sizes. Platform-independent tests also cover policy rejection with
a successful coverage resolver and policy changes after a positive/negative cache answer.
