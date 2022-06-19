uniform highp sampler2D u_texture;
uniform highp vec2      u_textureResolution;
uniform highp vec2      u_viewportResolution;

in highp vec2 fs_FragCoord;
out highp vec4 fragColor;

#define BlurSamples  64

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
    const int BlurSamplesSquared = BlurSamples * BlurSamples;

    for (int i = 0; i < BlurSamplesSquared; i++)
    {
        highp vec2 d = vec2(i % BlurSamples, i / BlurSamples) - 0.5 * float(BlurSamples);
        outputColor += gaussian(d) * texture(sp, uv + scale * d);
    }

    return outputColor / outputColor.a;
}

// @param BlurSize       radius (16 is a good value)
// @param BlurQuality    default 4.0 - more is better but slower
// @param BlurDirections default 16.0 - more is better but slower
highp vec4 blur2(highp sampler2D sp, highp vec2 uv, highp float BlurSize, highp float BlurQuality, highp float BlurDirections)
{
    highp vec2 radius = BlurSize / u_viewportResolution;
    highp vec4 colorContribution = texture(sp, uv);

    highp float TwoPi = 6.28318530718; // 2 * Pi
    highp float CircleStepIncrement = TwoPi / BlurDirections;
    highp float NeighborStepIncrement = 1.0 / BlurQuality;

    for (highp float d = 0.0; d < TwoPi; d += CircleStepIncrement)
        for (highp float i = NeighborStepIncrement; i <= 1.0; i += NeighborStepIncrement)
            colorContribution += texture(sp, uv + vec2(cos(d), sin(d)) * radius * i);

    // Output to screen
    return colorContribution / (BlurQuality * BlurDirections - 15.0);
}

void main()
{
    highp vec2 uv = vec2(gl_FragCoord.xy / u_viewportResolution);
    fragColor = blur2(u_texture, uv, 64.0, 8.0, 32.0);
    // fragColor = blur(u_texture, uv, 1.0 / u_textureResolution);
}
