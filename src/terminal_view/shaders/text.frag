layout (binding = 0) uniform mediump sampler2DArray fs_monochromeTextures;
layout (binding = 1) uniform mediump sampler2DArray fs_colorTextures;

in mediump vec3 fs_TexCoord; // TODO: split up into struct {vec3 TexCoord; float/*bool*/ monochrome; };
in mediump vec4 fs_textColor;

out mediump vec4 color;

void main()
{
    // ### monochrome glyph
    mediump float v = texture(fs_monochromeTextures, fs_TexCoord).r;
    mediump vec4 sampled = vec4(1.0, 1.0, 1.0, v);
    color = sampled * fs_textColor;
}
