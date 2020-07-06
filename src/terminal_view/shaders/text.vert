//layout (location = 0) uniform mat4 vs_projection;
uniform mat4 vs_projection;                         // projection matrix (flips around the coordinate system)
uniform vec2 vs_cellSize;                           // size of a single cell.
uniform vec2 vs_margin;                             // contains the left and bottom margin

layout (location = 0) in mediump vec3 vs_vertex;    // target vertex coordinates
layout (location = 1) in mediump vec4 vs_texCoords; // 3D-atlas texture coordinates
layout (location = 2) in mediump vec4 vs_colors;    // custom foreground colors

out mediump vec4 fs_TexCoord;
out mediump vec4 fs_textColor;

void main()
{
    #if 1
    gl_Position = vs_projection * vec4(vs_vertex.xyz, 1.0);
    #else
    //OLD: gl_Position = vs_projection * vec4(vs_vertex.xyz, 1.0);
    vec2 coord = vs_margin + vs_cellSize * vs_vertex.xy;
    gl_Position = vs_projection * vec4(coord, 0.0, 1.0);
    #endif

    fs_TexCoord = vs_texCoords;
    fs_textColor = vs_colors;
}
