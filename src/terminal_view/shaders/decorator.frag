// layout (binding = 0) uniform mediump sampler2DArray fs_decoratorTextures;
uniform mediump sampler2DArray fs_decoratorTextures;

in mediump vec4 fs_TexCoord;
in mediump vec4 fs_decoratorColor;

// layout (location = 0) out vec4 color;
out mediump vec4 color;

void main()
{
    mediump float v = texture(fs_decoratorTextures, fs_TexCoord.xyz).r;
    mediump vec4 sampled = vec4(1.0, 1.0, 1.0, v) * fs_decoratorColor;
    color = sampled;
}
