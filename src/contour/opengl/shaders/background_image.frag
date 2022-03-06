uniform mat4      u_projection;
uniform sampler2D u_backgroundImage;
uniform vec2      u_viewportResolution;
uniform vec2      u_backgroundResolution;
uniform float     u_blur;
uniform float     u_opacity;
uniform float     u_time;

in vec2 fs_TexCoord;

out vec4 fragColor;

// {{{ Gaussian blur
#define BlurSamples 128

float gaussian(vec2 i)
{
    const float TwoPi = 6.28318530718;
    const float Sigma = 0.25 * float(BlurSamples);
    const float SigmaSquared = Sigma * Sigma;
    vec2 iOverSigma = i / Sigma;
    return exp(-0.5 * dot(iOverSigma, iOverSigma)) / (TwoPi * SigmaSquared);
}

vec4 blur(sampler2D sp, vec2 uv, vec2 scale)
{
    vec4 outputColor = vec4(0);
    int s = BlurSamples;

    for ( int i = 0; i < s*s; i++ ) {
        vec2 d = vec2(i%s, i/s) - float(BlurSamples)/2.;
        outputColor += gaussian(d) * texture( sp, uv + scale * d);
    }

    return outputColor / outputColor.a;
}
// }}}

void main()
{
    if (u_blur >= 1.0)
        fragColor = blur(u_backgroundImage,
                         fs_TexCoord,
                         1.0 / u_backgroundResolution.xy);
    else
        fragColor = texture(u_backgroundImage, fs_TexCoord.xy);

    fragColor *= u_opacity;

    if (u_time <= FADE_TIME)
        fragColor.a *= u_time / FADE_TIME;
}
