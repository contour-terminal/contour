uniform highp mat4     u_projection;
uniform lowp sampler2D u_backgroundImage;
uniform highp vec2     u_resolution;
uniform highp float    u_blur;
uniform highp float    u_opacity;
uniform highp float    u_time;

layout (location = 0) in highp vec3 vs_vertex;    // target vertex coordinates
layout (location = 1) in highp vec2 vs_texCoords; // normalized texture coordinates

out highp vec2 fs_TexCoord;
out highp vec2 fs_FragCoord;

void main()
{
    gl_Position = u_projection * vec4(vs_vertex.xyz, 1.0);
    fs_TexCoord = vs_texCoords;
    fs_FragCoord = gl_Position.xy;
}
