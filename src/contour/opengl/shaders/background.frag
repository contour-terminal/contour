in vec4 fs_textColor;
out vec4 outColor;

uniform float u_time;

void main()
{
    outColor = fs_textColor;

    const float FadeTime = 3.0;
    if (u_time <= FadeTime)
        outColor *= u_time / FadeTime;
}
