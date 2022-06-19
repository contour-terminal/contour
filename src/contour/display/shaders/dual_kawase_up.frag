in highp vec4 vColor;
out highp vec4 fColor;

uniform highp sampler2D u_texture;
uniform highp vec2 u_viewportResolution;
uniform highp vec2 u_offset;
uniform highp vec2 u_halfpixel;

void main()
{
    highp vec2 uv = vec2(gl_FragCoord.xy / u_viewportResolution);

    highp vec4 sum = texture(u_texture, uv + vec2(-u_halfpixel.x * 2.0, 0.0) * u_offset);
    sum += texture(u_texture, uv + vec2(-u_halfpixel.x, u_halfpixel.y) * u_offset) * 2.0;
    sum += texture(u_texture, uv + vec2(0.0, u_halfpixel.y * 2.0) * u_offset);
    sum += texture(u_texture, uv + vec2(u_halfpixel.x, u_halfpixel.y) * u_offset) * 2.0;
    sum += texture(u_texture, uv + vec2(u_halfpixel.x * 2.0, 0.0) * u_offset);
    sum += texture(u_texture, uv + vec2(u_halfpixel.x, -u_halfpixel.y) * u_offset) * 2.0;
    sum += texture(u_texture, uv + vec2(0.0, -u_halfpixel.y * 2.0) * u_offset);
    sum += texture(u_texture, uv + vec2(-u_halfpixel.x, -u_halfpixel.y) * u_offset) * 2.0;

    fColor = sum / 12.0;
}
