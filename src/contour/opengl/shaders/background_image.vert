uniform mat4      u_projection;
uniform sampler2D u_backgroundImage;
uniform vec2      u_resolution;
uniform float     u_blur;
uniform float     u_opacity;
uniform float     u_time;

layout (location = 0) in vec3 vs_vertex;    // target vertex coordinates
layout (location = 1) in vec2 vs_texCoords; // 2D-atlas texture coordinates

out vec2 fs_TexCoord;
out vec2 fs_FragCoord;

void main()
{
    gl_Position = u_projection * vec4(vs_vertex.xyz, 1.0);
    fs_TexCoord = vs_texCoords;
    fs_FragCoord = gl_Position.xy;
}
