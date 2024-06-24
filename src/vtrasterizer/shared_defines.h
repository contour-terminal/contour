// NOLINTBEGIN(cppcoreguidelines-macro-to-enum)
// NOLINTBEGIN(modernize-macro-to-enum)

// Shared preprocessor definitions between C++ and GLSL.
//
// This file has no header guards because GLSL does not seem to understand that.

// Render a alpha-antialiased glyph tile using the red channel.
#define FRAGMENT_SELECTOR_GLYPH_ALPHA 0

// Render a raw BGRA image (e.g. emoji or image data)
#define FRAGMENT_SELECTOR_IMAGE_BGRA 1

// Render an LCD-subpixel antialiased glyph (simple algorithm)
#define FRAGMENT_SELECTOR_GLYPH_LCD_SIMPLE 2

// Render an LCD-subpixel antialiased glyph (advanced algorithm)
#define FRAGMENT_SELECTOR_GLYPH_LCD 3

// NOLINTEND(modernize-macro-to-enum)
// NOLINTEND(cppcoreguidelines-macro-to-enum)
