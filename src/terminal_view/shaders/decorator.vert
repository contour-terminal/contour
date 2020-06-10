//layout (location = 0) uniform mat4 vs_projection;
uniform mat4 vs_projection;

layout (location = 0) in mediump vec3 vs_vertex;    // target vertex coordinates
layout (location = 1) in mediump vec4 vs_texCoords; // 3D-atlas texture coordinates
layout (location = 2) in mediump vec4 vs_colors;    // custom decorator colors

out mediump vec4 fs_TexCoord;         // Atlas texture coordinates
out mediump vec4 fs_decoratorColor;   // decoration color

void main()
{
    gl_Position = vs_projection * vec4(vs_vertex.xyz, 1.0);

    fs_TexCoord = vs_texCoords;
    fs_decoratorColor = vs_colors;
}

