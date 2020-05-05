layout (binding = 0) uniform mediump sampler2DArray fs_monochromeTextures;
layout (binding = 1) uniform mediump sampler2DArray fs_colorTextures;

in mediump vec4 fs_TexCoord;
in mediump vec4 fs_textColor;

out mediump vec4 color;

void main()
{
    if (fs_TexCoord.w == 0.0)
    {
        // monochrome glyph (R)
        mediump float v = texture(fs_monochromeTextures, fs_TexCoord.xyz).r;
        mediump vec4 sampled = vec4(1.0, 1.0, 1.0, v);
        color = sampled * fs_textColor;
    }
    else
    {
        // colored glyph (RGBA)
        mediump vec4 tex = texture(fs_colorTextures, fs_TexCoord.xyz);
        color = tex; // * color.a;
    }
}
