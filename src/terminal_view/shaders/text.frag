// layout (binding = 0) uniform mediump sampler2DArray fs_monochromeTextures;
// layout (binding = 1) uniform mediump sampler2DArray fs_colorTextures;
uniform mediump sampler2DArray fs_monochromeTextures;
uniform mediump sampler2DArray fs_colorTextures;

in mediump vec4 fs_TexCoord;
in mediump vec4 fs_textColor;

// Dual source blending (since OpenGL 3.3)
// layout (location = 0, index = 0) out vec4 color;
// layout (location = 0, index = 1) out vec4 colorMask;
out mediump vec4 color;

void main()
{
    if (fs_TexCoord.w == 0.0)
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
    else
    {
        // colored glyph (RGBA)
        mediump vec4 v = texture(fs_colorTextures, fs_TexCoord.xyz);
        color = v;
    }
}
