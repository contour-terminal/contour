uniform float pixel_x;                  // 1.0 / lcdAtlas.width
uniform sampler2D fs_textureAtlas;      // RGBA
uniform float u_time;

in vec4 fs_TexCoord;
in vec4 fs_textColor;

// Dual source blending (since OpenGL 3.3)
// layout (location = 0, index = 0) out vec4 color;
// layout (location = 0, index = 1) out vec4 colorMask;
out vec4 fragColor;

const vec4 TEST_PIXEL = vec4(1.0, 0.0, 0.0, 1.0); // test pixel for debugging

void renderGrayscaleGlyph()
{
    // XXX monochrome glyph (RGB)
    //vec4 alphaMap = texture(fs_monochromeTextures, fs_TexCoord.xy);
    //fragColor = fs_textColor;
    //colorMask = alphaMap;

    // Using the RED-channel as alpha-mask of an anti-aliases glyph.
    vec4 pixel = texture(fs_textureAtlas, fs_TexCoord.xy);
    vec4 sampled = vec4(1.0, 1.0, 1.0, pixel.r);
    fragColor = sampled * fs_textColor;
}

// Renders an RGBA texture. This is used to render images (such as Sixel graphics or Emoji).
void renderColoredRGBA()
{
    // colored image (RGBA)
    vec4 v = texture(fs_textureAtlas, fs_TexCoord.xy);
    //v = TEST_PIXEL;
    fragColor = v;
}

// Simple LCD subpixel rendering will cause color fringes on the left/right side of the glyph
// shapes. People may be used to this already?
void renderLcdGlyphSimple()
{
    // LCD glyph (RGB)
    vec4 v = texture(fs_textureAtlas, fs_TexCoord.xy); // .rgb ?

    // float a = min(v.r, min(v.g, v.b));
    float a = (v.r + v.g + v.b) / 3.0;

    fragColor = vec4(v.rgb * fs_textColor.rgb, a);
}

// Calcualtes subpixel shifting.
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

    // maybe faster?
    //
    //    int ishift = int(shift * 100.0) / 33; // 0, 1, 2, 3
    //    switch (ishift) { case 0, 1, 2... }

    if (shift <= OneThird)
    {
        float z = shift / OneThird;
        r = mix(current.r, previous.b, z);
        g = mix(current.g, current.r,  z);
        b = mix(current.b, current.g,  z);
    }
    else if (shift <= TwoThird)
    {
        float z = (shift - OneThird) / OneThird;
        r = mix(previous.b, previous.g, z);
        g = mix(current.r,  previous.b, z);
        b = mix(current.g,  current.r,  z);
    }
    else if (shift < 1.0)
    {
        float z = (shift - TwoThird) / OneThird;
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
    float px = pixel_x;
    vec2 pixelOffset = vec2(1.0, 0.0) * px;
    //vec3 pixelOffset = vec3(1.0, 0.0, 0.0) * px;

    // LCD glyph (RGB)
    vec4 current  = texture(fs_textureAtlas, fs_TexCoord.xy);
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
    vec4 glyphContribution = vec4(r, g, b, rgbMin)          * rgbMaxNormComplement;
    vec4 color = glyphContribution + colorContribution;

    float alpha = color.a * fs_textColor.a;

    fragColor = vec4(color.rgb, alpha);
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
        case FRAGMENT_SELECTOR_GLYPH_ALPHA:
        default:
            renderGrayscaleGlyph();
            break;
    }

    const float FadeTime = 3.0;
    if (u_time <= FadeTime)
        fragColor *= u_time / FadeTime;
}
