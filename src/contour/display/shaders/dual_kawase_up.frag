#version 330
in highp vec4 vColor;
out highp vec4 fColor;

uniform sampler2D u_texture;
uniform vec2 u_viewportResolution;
uniform vec2 u_offset;
uniform vec2 u_halfpixel;

void main()
{
    vec2 uv = vec2(gl_FragCoord.xy / u_viewportResolution);

    vec4 sum = texture(u_texture, uv + vec2(-u_halfpixel.x * 2.0, 0.0) * u_offset);
    sum += texture(u_texture, uv + vec2(-u_halfpixel.x, u_halfpixel.y) * u_offset) * 2.0;
    sum += texture(u_texture, uv + vec2(0.0, u_halfpixel.y * 2.0) * u_offset);
    sum += texture(u_texture, uv + vec2(u_halfpixel.x, u_halfpixel.y) * u_offset) * 2.0;
    sum += texture(u_texture, uv + vec2(u_halfpixel.x * 2.0, 0.0) * u_offset);
    sum += texture(u_texture, uv + vec2(u_halfpixel.x, -u_halfpixel.y) * u_offset) * 2.0;
    sum += texture(u_texture, uv + vec2(0.0, -u_halfpixel.y * 2.0) * u_offset);
    sum += texture(u_texture, uv + vec2(-u_halfpixel.x, -u_halfpixel.y) * u_offset) * 2.0;

    fColor = sum / 12.0;
}
