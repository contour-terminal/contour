uniform mat4      u_projection;
uniform sampler2D u_backgroundImage;
uniform vec2      u_resolution;
uniform float     u_blur;
uniform float     u_opacity;
uniform float     u_time;

in vec2 fs_TexCoord;
in vec2 fs_FragCoord;

out vec4 fragColor;

vec4 blur()
{
    float BlurSize = 16.0;       // radius (16 is a good value)
    float BlurQuality = 4.0;     // default 4.0 - more is better but slower
    float BlurDirections = 16.0; // default 16.0 - more is better but slower

    vec2 radius = BlurSize / u_resolution.xy;
    vec2 uv = fs_TexCoord;
    vec4 colorContribution = texture(u_backgroundImage, uv);

    float TwoPi = 6.28318530718; // 2 * Pi
    float CircleStepIncrement = TwoPi / BlurDirections;
    float NeighborStepIncrement = 1.0 / BlurQuality;

    for (float d = 0.0; d < TwoPi; d += CircleStepIncrement)
    {
        for (float i = NeighborStepIncrement; i <= 1.0; i += NeighborStepIncrement)
        {
            vec2 uvOffset = vec2(cos(d), sin(d)) * radius * i;
            colorContribution += texture(u_backgroundImage, uv + uvOffset);
        }
    }

    // Output to screen
    return colorContribution / (BlurQuality * BlurDirections - 15.0);
}

void main()
{
    if (u_blur != 0.0)
        fragColor = blur();
    else
        fragColor = texture(u_backgroundImage, fs_TexCoord.xy);

    fragColor *= u_opacity;// * (cos(u_time) + 1.0) / 2.0;
}
