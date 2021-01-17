uniform mat4 vs_projection;                 // projection matrix (flips around the coordinate system)

layout (location = 0) in vec3 vs_vertex;    // target vertex coordinates
layout (location = 1) in vec4 vs_texCoords; // 3D-atlas texture coordinates
layout (location = 2) in vec4 vs_colors;    // custom foreground colors

out vec4 fs_TexCoord;
out vec4 fs_textColor;

void main()
{
    gl_Position = vs_projection * vec4(vs_vertex.xyz, 1.0);

    fs_TexCoord = vs_texCoords;
    fs_textColor = vs_colors;
}
