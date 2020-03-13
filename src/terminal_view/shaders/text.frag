#version 300 es

in mediump vec2 TexCoords;
out mediump vec4 color;

uniform mediump sampler2D text;
uniform mediump vec4 textColor;

void main()
{
    mediump vec4 sampled = vec4(1.0, 1.0, 1.0, texture2D(text, TexCoords).r);
    color = textColor * sampled;
}
