#version 330

uniform sampler2D u_texture;
uniform vec2      u_textureResolution;
uniform vec2      u_viewportResolution;

in lowp vec2 fs_FragCoord;
out lowp vec4 fragColor;

#define BlurSamples  64

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
    const int BlurSamplesSquared = BlurSamples * BlurSamples;

    for (int i = 0; i < BlurSamplesSquared; i++)
    {
        vec2 d = vec2(i % BlurSamples, i / BlurSamples) - 0.5 * float(BlurSamples);
        outputColor += gaussian(d) * texture(sp, uv + scale * d);
    }

    return outputColor / outputColor.a;
}

// @param BlurSize       radius (16 is a good value)
// @param BlurQuality    default 4.0 - more is better but slower
// @param BlurDirections default 16.0 - more is better but slower
vec4 blur2(sampler2D sp, vec2 uv, float BlurSize, float BlurQuality, float BlurDirections)
{
    vec2 radius = BlurSize / u_viewportResolution;
    vec4 colorContribution = texture(sp, uv);

    float TwoPi = 6.28318530718; // 2 * Pi
    float CircleStepIncrement = TwoPi / BlurDirections;
    float NeighborStepIncrement = 1.0 / BlurQuality;

    for (float d = 0.0; d < TwoPi; d += CircleStepIncrement)
        for (float i = NeighborStepIncrement; i <= 1.0; i += NeighborStepIncrement)
            colorContribution += texture(sp, uv + vec2(cos(d), sin(d)) * radius * i);

    // Output to screen
    return colorContribution / (BlurQuality * BlurDirections - 15.0);
}

void main()
{
    vec2 uv = vec2(gl_FragCoord.xy / u_viewportResolution);
    fragColor = blur2(u_texture, uv, 64, 8, 32);
    // fragColor = blur(u_texture, uv, 1.0 / u_textureResolution);
}
