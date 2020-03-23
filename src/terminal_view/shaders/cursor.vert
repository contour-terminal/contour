in mediump vec2 position;
uniform mediump mat4 u_transform;

void main()
{
    gl_Position = u_transform * vec4(position, 0.2, 1.0);
}
