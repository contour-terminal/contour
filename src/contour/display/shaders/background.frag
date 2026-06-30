#version 440

// Qt RHI (Vulkan-style GLSL) fragment shader for the terminal background/rect pass.
//
// Uniform block layout (std140, binding=0). Must match background.vert exactly so
// a single uniform buffer feeds both stages.
//
//   field         | type   | std140 offset | size (bytes)
//   --------------+--------+---------------+-------------
//   u_transform   | mat4   |  0            | 64
//   u_time        | float  | 64            |  4
//                 |        | (pad to 80, block size = 80)
layout(std140, binding = 0) uniform Buf
{
    mat4 u_transform;
    float u_time;
}
ubuf;

layout(location = 0) in vec4 fs_textColor;

layout(location = 0) out vec4 outColor;

void main()
{
    outColor = fs_textColor;
}
