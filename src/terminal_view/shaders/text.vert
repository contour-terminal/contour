#version 300 es
precision mediump float;
in mediump vec4 vertex;
out mediump vec2 TexCoords;

uniform mediump mat4 projection;

void main()
{
    gl_Position = projection * vec4(vertex.xy, 0.1, 1.0);
    TexCoords = vertex.zw;
}

