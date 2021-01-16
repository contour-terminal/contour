// layout (binding = 0) uniform mediump sampler2DArray fs_monochromeTextures;   // R
// layout (binding = 1) uniform mediump sampler2DArray fs_colorTextures;        // RGBA
// layout (binding = 2) uniform mediump sampler2DArray fs_lcdTexture;           // RGB
uniform mediump sampler2DArray fs_monochromeTextures; // R
uniform mediump sampler2DArray fs_colorTextures;      // RGBA
uniform mediump sampler2DArray fs_lcdTexture;         // RGB

in mediump vec4 fs_TexCoord;
in mediump vec4 fs_textColor;

// Dual source blending (since OpenGL 3.3)
// layout (location = 0, index = 0) out vec4 color;
// layout (location = 0, index = 1) out vec4 colorMask;
out mediump vec4 color;

void renderGray()
{
    // XXX monochrome glyph (RGB)
    //mediump vec4 alphaMap = texture(fs_monochromeTextures, fs_TexCoord.xyz);
    //color = fs_textColor;
    //colorMask = alphaMap;

    // when only using the RED-channel
    mediump float v = texture(fs_monochromeTextures, fs_TexCoord.xyz).r;
    mediump vec4 sampled = vec4(1.0, 1.0, 1.0, v);
    color = sampled * fs_textColor;
}

void renderColoredRGBA()
{
    // colored glyph (RGBA)
    mediump vec4 v = texture(fs_colorTextures, fs_TexCoord.xyz);
    color = v;
}

void renderLcdRGB()
{
    // LCD glyph (RGB)
    mediump vec4 v = texture(fs_lcdTexture, fs_TexCoord.xyz); // .rgb ?
    mediump float a = min(v.r, min(v.g, v.b));
    v = v * fs_textColor;
    v.a = a;
    color = v;
}

void main()
{
    switch (int(fs_TexCoord.w))
    {
        case 2:
            renderLcdRGB();
            break;
        case 1:
            renderColoredRGBA();
            break;
        case 0:
        default:
            renderGray();
            break;
    }
}
