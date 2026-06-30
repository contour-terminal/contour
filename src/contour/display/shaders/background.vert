#version 440

// Qt RHI (Vulkan-style GLSL) vertex shader for the terminal background/rect pass.
//
// Uniform block layout (std140, binding=0). Shared by background.vert and
// background.frag so a single uniform buffer feeds both stages.
//
//   field         | type   | std140 offset | size (bytes)
//   --------------+--------+---------------+-------------
//   u_transform   | mat4   |  0            | 64
//   u_time        | float  | 64            |  4
//                 |        | (pad to 80, block size = 80)
//
// u_transform is the item-local -> clip-space matrix (formerly u_projection).
layout(std140, binding = 0) uniform Buf
{
    mat4 u_transform;
    float u_time;
}
ubuf;

layout(location = 0) in vec3 vs_vertex; // target vertex coordinates
layout(location = 1) in vec4 vs_colors; // custom foreground colors

layout(location = 0) out vec4 fs_textColor;

void main()
{
    gl_Position = ubuf.u_transform * vec4(vs_vertex.xyz, 1.0);
    fs_textColor = vs_colors;
}
