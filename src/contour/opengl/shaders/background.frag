in vec4 fs_textColor;
out vec4 outColor;

uniform float u_time;

void main()
{
    outColor = fs_textColor;

    if (u_time <= FADE_TIME)
        outColor *= u_time / FADE_TIME;
}
