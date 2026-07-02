#version 440

// Qt RHI (Vulkan-style GLSL) fragment shader for the terminal text/glyph pass.
//
// Uniform block layout (std140, binding=0). Must match text.vert exactly so a
// single uniform buffer feeds both stages.
//
//   field               | type   | std140 offset | size (bytes)
//   --------------------+--------+---------------+-------------
//   u_transform         | mat4   |  0            | 64
//   u_time              | float  | 64            |  4
//   pixel_x             | float  | 68            |  4   (1.0 / lcdAtlas.width)
//   (pad before vec4)   |        | 72            |  8   (vec4 needs 16-byte align)
//   u_textOutlineColor  | vec4   | 80            | 16   (outline RGBA color)
//                       |        | (block size = 96)
layout(std140, binding = 0) uniform Buf
{
    mat4 u_transform;
    float u_time;
    float pixel_x;
    vec4 u_textOutlineColor;
}
ubuf;

// RGBA glyph/image atlas texture.
layout(binding = 1) uniform sampler2D fs_textureAtlas;

layout(location = 0) in vec4 fs_TexCoord;
layout(location = 1) in vec4 fs_textColor;

layout(location = 0) out vec4 fragColor;

// Fragment render-mode selectors (inlined from vtrasterizer/shared_defines.h).
const int FRAGMENT_SELECTOR_GLYPH_ALPHA = 0;      // alpha-antialiased glyph (red channel)
const int FRAGMENT_SELECTOR_IMAGE_BGRA = 1;       // raw BGRA image (emoji / image data)
const int FRAGMENT_SELECTOR_GLYPH_LCD_SIMPLE = 2; // LCD-subpixel glyph (simple algorithm)
const int FRAGMENT_SELECTOR_GLYPH_LCD = 3;        // LCD-subpixel glyph (advanced algorithm)
const int FRAGMENT_SELECTOR_GLYPH_OUTLINED = 4;   // pre-rasterized outline (R=fill, G=outline, A=max)

void renderGrayscaleGlyph()
{
    // Using the RED-channel as alpha-mask of an anti-aliased glyph.
    vec4 pixel = texture(fs_textureAtlas, fs_TexCoord.xy);
    float glyphAlpha = pixel.r;

    vec4 sampled = vec4(1.0, 1.0, 1.0, glyphAlpha);
    fragColor = sampled * fs_textColor;
}

// Renders an RGBA texture. This is used to render images (such as Sixel graphics or Emoji).
void renderColoredRGBA()
{
    // colored image (RGBA)
    vec4 v = texture(fs_textureAtlas, fs_TexCoord.xy);
    fragColor = v;
}

// Simple LCD subpixel rendering will cause color fringes on the left/right side of the glyph
// shapes. People may be used to this already?
void renderLcdGlyphSimple()
{
    // LCD glyph (RGB)
    vec4 v = texture(fs_textureAtlas, fs_TexCoord.xy); // .rgb ?

    float a = (v.r + v.g + v.b) / 3.0;

    fragColor = vec4(v.rgb * fs_textColor.rgb, a);
}

// Calculates subpixel shifting.
//
// @param current       current pixel to render
// @param previous      previous pixel, left neighbor of current.
// @param shift         fraction of a pixel to shift in range [0.0 .. 1.0)
//
// @return the shifted pixel
//
vec3 lcdPixelShift(vec3 current, vec3 previous, float shift)
{
    const float OneThird = 1.0 / 3.0;
    const float TwoThird = 2.0 / 3.0;

    float r = current.r;
    float g = current.g;
    float b = current.b;

    if (shift <= OneThird)
    {
        float z = shift / OneThird;
        r = mix(current.r, previous.b, z);
        g = mix(current.g, current.r, z);
        b = mix(current.b, current.g, z);
    }
    else if (shift <= TwoThird)
    {
        float z = (shift - OneThird) / OneThird;
        r = mix(previous.b, previous.g, z);
        g = mix(current.r, previous.b, z);
        b = mix(current.g, current.r, z);
    }
    else if (shift < 1.0)
    {
        float z = (shift - TwoThird) / OneThird;
        r = mix(previous.g, previous.r, z);
        g = mix(previous.b, previous.g, z);
        b = mix(current.r, previous.b, z);
    }

    return vec3(r, g, b);
}

// Renders the LCD subpixel optimized glyph as described in:
//     Nicolas P. Rougier, Higher Quality 2D Text Rendering,
//     Journal of Computer Graphics Techniques (JCGT), vol. 2, no. 1, 50-64, 2013
// See:
//     http://jcgt.org/published/0002/01/04/
void renderLcdGlyph()
{
    float px = ubuf.pixel_x;
    vec2 pixelOffset = vec2(1.0, 0.0) * px;

    // LCD glyph (RGB)
    vec4 current = texture(fs_textureAtlas, fs_TexCoord.xy);
    vec4 previous = texture(fs_textureAtlas, fs_TexCoord.xy - pixelOffset);

    // The text in a terminal does enforce fixed-width advances, and therefore
    // rendering a glyph should always start at a full pixel with no shift.
    //
    // We keep this variable here anyways for clearance.
    const float shift = 0.0;
    vec3 shifted = lcdPixelShift(current.rgb, previous.rgb, shift);

    float r = shifted.r;
    float g = shifted.g;
    float b = shifted.b;

    float rgbAvg = (r + g + b) / 3.0;
    float rgbMin = min(min(r, g), b);
    float rgbMax = max(max(r, g), b);
    float rgbMaxNormComplement = 1.0 - rgbMax;

    vec4 colorContribution = vec4(fs_textColor.rgb, rgbAvg) * rgbMax;
    vec4 glyphContribution = vec4(r, g, b, rgbMin) * rgbMaxNormComplement;
    vec4 color = glyphContribution + colorContribution;

    float alpha = color.a * fs_textColor.a;

    fragColor = vec4(color.rgb, alpha);
}

// Renders a glyph with pre-rasterized outline via FT_Stroker.
// R = fill alpha, G = outline alpha, B = unused, A = max(fill, outline).
void renderOutlinedGlyph()
{
    vec4 pixel = texture(fs_textureAtlas, fs_TexCoord.xy);
    float fillAlpha = pixel.r;
    float outlineAlpha = pixel.g;

    vec4 glyph = vec4(fs_textColor.rgb, fs_textColor.a * fillAlpha);
    vec4 outline = vec4(ubuf.u_textOutlineColor.rgb, ubuf.u_textOutlineColor.a * outlineAlpha);

    // Composite glyph over outline ("over" alpha blending)
    fragColor.a = glyph.a + outline.a * (1.0 - glyph.a);
    if (fragColor.a > 0.0)
        fragColor.rgb = (glyph.rgb * glyph.a + outline.rgb * outline.a * (1.0 - glyph.a)) / fragColor.a;
    else
        fragColor = vec4(0.0);
}

void main()
{
    int selector = int(fs_TexCoord.w); // This is the RenderTile::userdata component.

    switch (selector)
    {
        case FRAGMENT_SELECTOR_GLYPH_LCD:
            renderLcdGlyph();
            break;
        case FRAGMENT_SELECTOR_GLYPH_LCD_SIMPLE:
            renderLcdGlyphSimple();
            break;
        case FRAGMENT_SELECTOR_IMAGE_BGRA:
            renderColoredRGBA();
            break;
        case FRAGMENT_SELECTOR_GLYPH_OUTLINED:
            renderOutlinedGlyph();
            break;
        case FRAGMENT_SELECTOR_GLYPH_ALPHA:
        default:
            renderGrayscaleGlyph();
            break;
    }
}
