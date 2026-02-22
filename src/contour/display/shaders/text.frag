uniform highp float pixel_x;                  // 1.0 / lcdAtlas.width
uniform highp sampler2D fs_textureAtlas;      // RGBA
uniform highp float u_time;
uniform highp vec4 u_textOutlineColor;        // outline RGBA color

in highp vec4 fs_TexCoord;
in highp vec4 fs_textColor;

// Dual source blending (since OpenGL 3.3)
// layout (location = 0, index = 0) out highp vec4 color;
// layout (location = 0, index = 1) out highp vec4 colorMask;
out highp vec4 fragColor;

const highp vec4 TEST_PIXEL = vec4(1.0, 0.0, 0.0, 1.0); // test pixel for debugging

void renderGrayscaleGlyph()
{
    // Using the RED-channel as alpha-mask of an anti-aliased glyph.
    highp vec4 pixel = texture(fs_textureAtlas, fs_TexCoord.xy);
    highp float glyphAlpha = pixel.r;

    highp vec4 sampled = vec4(1.0, 1.0, 1.0, glyphAlpha);
    fragColor = sampled * fs_textColor;
}

// Renders an RGBA texture. This is used to render images (such as Sixel graphics or Emoji).
void renderColoredRGBA()
{
    // colored image (RGBA)
    highp vec4 v = texture(fs_textureAtlas, fs_TexCoord.xy);
    //v = TEST_PIXEL;
    fragColor = v;
}

// Simple LCD subpixel rendering will cause color fringes on the left/right side of the glyph
// shapes. People may be used to this already?
void renderLcdGlyphSimple()
{
    // LCD glyph (RGB)
    highp vec4 v = texture(fs_textureAtlas, fs_TexCoord.xy); // .rgb ?

    // float a = min(v.r, min(v.g, v.b));
    highp float a = (v.r + v.g + v.b) / 3.0;

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
highp vec3 lcdPixelShift(highp vec3 current, highp vec3 previous, highp float shift)
{
    const highp float OneThird = 1.0 / 3.0;
    const highp float TwoThird = 2.0 / 3.0;

    highp float r = current.r;
    highp float g = current.g;
    highp float b = current.b;

    // maybe faster?
    //
    //    int ishift = int(shift * 100.0) / 33; // 0, 1, 2, 3
    //    switch (ishift) { case 0, 1, 2... }

    if (shift <= OneThird)
    {
        highp float z = shift / OneThird;
        r = mix(current.r, previous.b, z);
        g = mix(current.g, current.r,  z);
        b = mix(current.b, current.g,  z);
    }
    else if (shift <= TwoThird)
    {
        highp float z = (shift - OneThird) / OneThird;
        r = mix(previous.b, previous.g, z);
        g = mix(current.r,  previous.b, z);
        b = mix(current.g,  current.r,  z);
    }
    else if (shift < 1.0)
    {
        highp float z = (shift - TwoThird) / OneThird;
        r = mix(previous.g, previous.r, z);
        g = mix(previous.b, previous.g, z);
        b = mix(current.r,  previous.b, z);
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
    highp float px = pixel_x;
    highp vec2 pixelOffset = vec2(1.0, 0.0) * px;
    //highp vec3 pixelOffset = vec3(1.0, 0.0, 0.0) * px;

    // LCD glyph (RGB)
    highp vec4 current  = texture(fs_textureAtlas, fs_TexCoord.xy);
    highp vec4 previous = texture(fs_textureAtlas, fs_TexCoord.xy - pixelOffset);

    // The text in a terminal does enforce fixed-width advances, and therefore
    // rendering a glyph should always start at a full pixel with no shift.
    //
    // We keep this variable here anyways for clearance.
    const highp float shift = 0.0;
    highp vec3 shifted = lcdPixelShift(current.rgb, previous.rgb, shift);

    highp float r = shifted.r;
    highp float g = shifted.g;
    highp float b = shifted.b;

    highp float rgbAvg = (r + g + b) / 3.0;
    highp float rgbMin = min(min(r, g), b);
    highp float rgbMax = max(max(r, g), b);
    highp float rgbMaxNormComplement = 1.0 - rgbMax;

    highp vec4 colorContribution = vec4(fs_textColor.rgb, rgbAvg) * rgbMax;
    highp vec4 glyphContribution = vec4(r, g, b, rgbMin)          * rgbMaxNormComplement;
    highp vec4 color = glyphContribution + colorContribution;

    highp float alpha = color.a * fs_textColor.a;

    fragColor = vec4(color.rgb, alpha);
}

// Renders a glyph with pre-rasterized outline via FT_Stroker.
// R = fill alpha, G = outline alpha, B = unused, A = max(fill, outline).
void renderOutlinedGlyph()
{
    highp vec4 pixel = texture(fs_textureAtlas, fs_TexCoord.xy);
    highp float fillAlpha = pixel.r;
    highp float outlineAlpha = pixel.g;

    highp vec4 glyph = vec4(fs_textColor.rgb, fs_textColor.a * fillAlpha);
    highp vec4 outline = vec4(u_textOutlineColor.rgb, u_textOutlineColor.a * outlineAlpha);

    // Composite glyph over outline ("over" alpha blending)
    fragColor.a = glyph.a + outline.a * (1.0 - glyph.a);
    if (fragColor.a > 0.0)
        fragColor.rgb = (glyph.rgb * glyph.a + outline.rgb * outline.a * (1.0 - glyph.a))
                        / fragColor.a;
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
