uniform highp mat4      u_projection;
uniform lowp sampler2D  u_backgroundImage;
uniform highp vec2      u_viewportResolution;
uniform highp vec2      u_backgroundResolution;
uniform highp float     u_blur;
uniform highp float     u_opacity;
uniform highp float     u_time;

in highp vec2 fs_TexCoord;

out highp vec4 fragColor;

// {{{ Gaussian blur
#define BlurSamples 128

highp float gaussian(highp vec2 i)
{
    const highp float TwoPi = 6.28318530718;
    const highp float Sigma = 0.25 * float(BlurSamples);
    const highp float SigmaSquared = Sigma * Sigma;
    highp vec2 iOverSigma = i / Sigma;
    return exp(-0.5 * dot(iOverSigma, iOverSigma)) / (TwoPi * SigmaSquared);
}

highp vec4 blur(highp sampler2D sp, highp vec2 uv, highp vec2 scale)
{
    highp vec4 outputColor = vec4(0);
    int s = BlurSamples;

    for ( int i = 0; i < s*s; i++ ) {
        highp vec2 d = vec2(i%s, i/s) - float(BlurSamples)/2.;
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
}
