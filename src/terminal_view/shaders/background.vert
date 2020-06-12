uniform mat4 u_projection;
layout (location = 0) in mediump vec3 vs_vertex;    // target vertex coordinates
layout (location = 1) in mediump vec4 vs_colors;    // custom foreground colors

out mediump vec4 fs_textColor;

void main()
{
    gl_Position = u_projection * vec4(vs_vertex.xyz, 1.0);
    fs_textColor = vs_colors;
}
