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

    vec4 sum = texture(u_texture, uv) * 4.0;
    sum += texture(u_texture, uv - u_halfpixel.xy * u_offset);
    sum += texture(u_texture, uv + u_halfpixel.xy * u_offset);
    sum += texture(u_texture, uv + vec2(u_halfpixel.x, -u_halfpixel.y) * u_offset);
    sum += texture(u_texture, uv - vec2(u_halfpixel.x, -u_halfpixel.y) * u_offset);

    fColor = sum / 8.0;
}
