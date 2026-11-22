#version 440

// Qt RHI (Vulkan-style GLSL) fragment shader for the image pass: one whole image (sixel, kitty, iTerm2, ...)
// drawn into its cells at whatever size its placement asks for. Paired with text.vert, whose vertex layout
// and uniform block it shares; the uniform block itself is not read here.
//
// Shrunk, each pixel averages the texels its footprint covers, as a smooth downscale does. Nearest
// sampling keeps one texel in N and drops the rest (text in a picture turns jagged), and trilinear
// filtering blurs badly between 1x and 2x, the ratio a 2x-rendered picture usually lands at. The
// footprint comes from the screen-space derivatives; it is covered by up to 4x4 bilinear taps at the mip
// level where it spans at most four texels. A bilinear tap already averages about a texel around it, so
// the taps spread over the footprint less half a texel: spread over all of it, the result is visibly
// softer than a box filter.
//
// At 1:1 and when enlarging, each pixel shows the texel it falls in, as before mipmapping came in: an
// enlarged sprite or a small sixel keeps its hard pixel edges, and a picture at 1:1 comes out texel for
// texel. A bilinear tap at the texel's centre is that texel exactly, which works without texelFetch.
//
// The texture holds premultiplied alpha, so the average weighs each texel by its coverage: averaging
// straight alpha would pull the colour of transparent texels (usually black) into the edges of opaque
// ones. The pass blends premultiplied (One, OneMinusSrcAlpha), so the average is the output as it is.

// The image, sampled linearly with a mip chain.
layout(binding = 1) uniform sampler2D fs_image;

// The image's size in texels (.xy). Supplied rather than queried: textureSize does not exist in every
// shading language this is baked for.
layout(std140, binding = 2) uniform ImageBuf
{
    vec4 u_imageSize;
}
imageBuf;

layout(location = 0) in vec4 fs_TexCoord;  // .xy: normalized texture coordinates
layout(location = 1) in vec4 fs_textColor; // the quad's tint, straight alpha

layout(location = 0) out vec4 fragColor;

void main()
{
    vec2 uv = fs_TexCoord.xy;
    // One pixel, in normalized texture coordinates, per axis (image quads are axis-aligned).
    vec2 pixel = max(abs(dFdx(uv)), abs(dFdy(uv)));
    vec2 footprint = pixel * imageBuf.u_imageSize.xy; // in texels of mip level 0
    vec4 tint = vec4(fs_textColor.rgb * fs_textColor.a, fs_textColor.a); // straight tint, premultiplied

    if (max(footprint.x, footprint.y) <= 1.01)
    {
        vec2 centre = (floor(uv * imageBuf.u_imageSize.xy) + 0.5) / imageBuf.u_imageSize.xy;
        fragColor = textureLod(fs_image, centre, 0.0) * tint;
        return;
    }

    // The mip level where the footprint spans at most four texels, and the footprint in its texels.
    float level = max(ceil(log2(max(footprint.x, footprint.y) / 4.0)), 0.0);
    vec2 texels = footprint / exp2(level);
    vec2 taps = clamp(ceil(texels - 0.01), vec2(1.0), vec2(4.0));
    vec2 spread = max(texels - 0.5, vec2(0.0)) / max(texels, vec2(1.0)) * pixel; // finite at a zero footprint

    vec4 sum = vec4(0.0);
    for (int i = 0; i < 4; ++i)
    {
        if (float(i) >= taps.x)
            break;
        for (int j = 0; j < 4; ++j)
        {
            if (float(j) >= taps.y)
                break;
            vec2 offset = ((vec2(float(i), float(j)) + 0.5) / taps - 0.5) * spread;
            sum += textureLod(fs_image, uv + offset, level);
        }
    }

    fragColor = sum / (taps.x * taps.y) * tint;
}
