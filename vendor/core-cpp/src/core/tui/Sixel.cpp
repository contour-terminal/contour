// SPDX-License-Identifier: Apache-2.0
#include <core/tui/Sixel.hpp>

#include <algorithm>
#include <format>
#include <limits>
#include <ranges>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace core::tui
{

namespace
{
    struct RgbPixel
    {
        std::uint8_t r = 0;
        std::uint8_t g = 0;
        std::uint8_t b = 0;
    };

    struct ColorBucket
    {
        std::vector<RgbPixel> pixels;
    };

    /// @brief Finds the channel (0=R, 1=G, 2=B) with the widest range in the bucket.
    auto widestChannel(ColorBucket const& bucket) -> int
    {
        auto minR = std::uint8_t { 255 };
        auto maxR = std::uint8_t { 0 };
        auto minG = std::uint8_t { 255 };
        auto maxG = std::uint8_t { 0 };
        auto minB = std::uint8_t { 255 };
        auto maxB = std::uint8_t { 0 };
        for (auto const& p: bucket.pixels)
        {
            minR = std::min(minR, p.r);
            maxR = std::max(maxR, p.r);
            minG = std::min(minG, p.g);
            maxG = std::max(maxG, p.g);
            minB = std::min(minB, p.b);
            maxB = std::max(maxB, p.b);
        }
        auto const rangeR = maxR - minR;
        auto const rangeG = maxG - minG;
        auto const rangeB = maxB - minB;
        if (rangeR >= rangeG && rangeR >= rangeB)
            return 0;
        if (rangeG >= rangeR && rangeG >= rangeB)
            return 1;
        return 2;
    }

    /// @brief Computes the average color of a bucket.
    auto averageColor(ColorBucket const& bucket) -> RgbPixel
    {
        if (bucket.pixels.empty())
            return {};
        auto sumR = std::uint64_t { 0 };
        auto sumG = std::uint64_t { 0 };
        auto sumB = std::uint64_t { 0 };
        for (auto const& p: bucket.pixels)
        {
            sumR += p.r;
            sumG += p.g;
            sumB += p.b;
        }
        auto const n = static_cast<std::uint64_t>(bucket.pixels.size());
        auto result = RgbPixel {};
        result.r = static_cast<std::uint8_t>(sumR / n);
        result.g = static_cast<std::uint8_t>(sumG / n);
        result.b = static_cast<std::uint8_t>(sumB / n);
        return result;
    }

    /// @brief Median-cut color quantization.
    /// @param pixels The input pixels.
    /// @param maxColors Target palette size.
    /// @return Quantized palette.
    auto medianCut(std::vector<RgbPixel> const& pixels, int maxColors) -> std::vector<RgbPixel>
    {
        auto buckets = std::vector<ColorBucket> {};
        buckets.push_back(ColorBucket { .pixels = pixels });

        while (std::cmp_less(buckets.size(), maxColors))
        {
            // Find the bucket with the most pixels
            auto largestIdx = std::size_t { 0 };
            auto largestSize = std::size_t { 0 };
            for (auto const i: std::views::iota(0uz, buckets.size()))
            {
                if (buckets[i].pixels.size() > largestSize)
                {
                    largestSize = buckets[i].pixels.size();
                    largestIdx = i;
                }
            }

            if (largestSize <= 1)
                break;

            auto& bucket = buckets[largestIdx];
            auto const channel = widestChannel(bucket);

            // Sort by the widest channel, then by the other two: a total order on the whole pixel.
            //
            // Sorting on the widest channel alone left pixels equal in it and different in the others
            // in whatever order the unstable sort produced, and the median split below then put
            // different pixels in each half. So one image quantized to a different palette depending
            // on the order its pixels arrived in, and encoded to different bytes on libstdc++, libc++
            // and MSVC's library. Pixels equal in all three channels are interchangeable, so no further
            // tiebreak can change what lands in either half -- and a stable sort would not do instead,
            // because it keeps the arrival order this is removing.
            std::ranges::sort(bucket.pixels, {}, [channel](RgbPixel const& p) {
                switch (channel)
                {
                    case 0: return std::tuple { p.r, p.g, p.b };
                    case 1: return std::tuple { p.g, p.r, p.b };
                    default: return std::tuple { p.b, p.r, p.g };
                }
            });

            // Split at the median
            auto const mid = bucket.pixels.size() / 2;
            auto newBucket = ColorBucket {};
            newBucket.pixels.assign(bucket.pixels.begin() + static_cast<std::ptrdiff_t>(mid),
                                    bucket.pixels.end());
            bucket.pixels.resize(mid);
            buckets.push_back(std::move(newBucket));
        }

        auto palette = std::vector<RgbPixel> {};
        palette.reserve(buckets.size());
        for (auto const& bucket: buckets)
            palette.push_back(averageColor(bucket));
        return palette;
    }

    /// @brief Finds the closest palette color to the given pixel using squared Euclidean distance.
    /// @param pixel The pixel to match.
    /// @param palette The color palette.
    /// @return Index of the closest palette entry.
    auto closestColor(RgbPixel const& pixel, std::vector<RgbPixel> const& palette) -> int
    {
        auto bestIdx = std::size_t { 0 };
        auto bestDist = std::numeric_limits<int>::max();
        for (auto const i: std::views::iota(0uz, palette.size()))
        {
            auto const& c = palette[i];
            auto const dr = static_cast<int>(pixel.r) - static_cast<int>(c.r);
            auto const dg = static_cast<int>(pixel.g) - static_cast<int>(c.g);
            auto const db = static_cast<int>(pixel.b) - static_cast<int>(c.b);
            auto const dist = (dr * dr) + (dg * dg) + (db * db);
            if (dist < bestDist)
            {
                bestDist = dist;
                bestIdx = i;
            }
        }
        return static_cast<int>(bestIdx);
    }

    /// @brief Emits a sixel row with run-length encoding.
    ///
    /// Sixel RLE format: `!count<char>` for runs of identical bytes.
    /// Falls back to raw characters for short runs (< 4).
    ///
    /// @param out The output string to append to.
    /// @param row The sixel byte values for one color across the band width.
    void emitSixelRowRLE(std::string& out, std::span<std::uint8_t const> row)
    {
        auto i = std::size_t { 0 };
        while (i < row.size())
        {
            auto const value = row[i];
            auto const ch = static_cast<char>(value + 63);
            auto runLen = std::size_t { 1 };
            while (i + runLen < row.size() && row[i + runLen] == value)
                ++runLen;
            if (runLen >= 4)
                out += std::format("!{}{}", runLen, ch);
            else
                out.append(runLen, ch);
            i += runLen;
        }
    }
} // namespace

auto encodeSixel(ImageData const& image, int maxColors) -> Result<std::string>
{
    if (image.width <= 0 || image.height <= 0)
        return std::unexpected(std::string("Image dimensions must be positive"));

    auto const pixelCount = static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height);
    if (image.pixels.size() < pixelCount * 4)
        return std::unexpected(std::string("Pixel data too small for given dimensions"));

    maxColors = std::clamp(maxColors, 1, 256);

    // Extract RGB pixels, marking transparent ones (alpha < 128) as index -1
    auto opaquePixels = std::vector<RgbPixel> {};
    opaquePixels.reserve(pixelCount);
    auto indexed = std::vector<int>(pixelCount, -1);

    for (auto const i: std::views::iota(0uz, pixelCount))
    {
        auto const alpha = image.pixels[(i * 4) + 3];
        if (alpha < 128)
            continue; // Transparent — leave indexed[i] = -1

        auto pixel = RgbPixel {};
        pixel.r = image.pixels[i * 4];
        pixel.g = image.pixels[(i * 4) + 1];
        pixel.b = image.pixels[(i * 4) + 2];
        opaquePixels.push_back(pixel);
        indexed[i] = 0; // Placeholder — will be mapped to palette after quantization
    }

    if (opaquePixels.empty())
        return std::string {}; // Fully transparent image

    // Quantize colors (only opaque pixels)
    auto const palette = medianCut(opaquePixels, maxColors);
    auto const paletteSize = static_cast<int>(palette.size());

    // 15-bit RGB color cache: 5 bits per channel → 32768 entries
    // Avoids repeated O(palette.size()) linear scans for similar pixel colors.
    auto colorCache = std::vector<int>(32768, -1);
    auto cachedClosestColor = [&](RgbPixel const& pixel) -> int {
        auto const key = ((pixel.r >> 3) << 10) | ((pixel.g >> 3) << 5) | (pixel.b >> 3);
        if (colorCache[static_cast<std::size_t>(key)] < 0)
            colorCache[static_cast<std::size_t>(key)] = closestColor(pixel, palette);
        return colorCache[static_cast<std::size_t>(key)];
    };

    // Map each opaque pixel to a palette index (with cache)
    for (auto const i: std::views::iota(0uz, pixelCount))
    {
        if (indexed[i] < 0)
            continue; // Transparent
        auto pixel = RgbPixel {};
        pixel.r = image.pixels[i * 4];
        pixel.g = image.pixels[(i * 4) + 1];
        pixel.b = image.pixels[(i * 4) + 2];
        indexed[i] = cachedClosestColor(pixel);
    }

    // Build sixel output with pre-allocated buffer
    auto result = std::string {};
    result.reserve(static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height));

    // Raster attributes: "Pan;Pad;Ph;Pv — tells terminal exact image dimensions
    result += std::format("\"1;1;{};{}", image.width, image.height);

    // Define palette: #idx;2;r%;g%;b% (percentage 0-100)
    for (auto const i: std::views::iota(0, paletteSize))
    {
        auto const& c = palette[static_cast<std::size_t>(i)];
        result += std::format("#{};2;{};{};{}", i, c.r * 100 / 255, c.g * 100 / 255, c.b * 100 / 255);
    }

    // Encode sixel data: single-pass per band with RLE compression
    auto const width = image.width;
    auto const height = image.height;
    auto const widthU = static_cast<std::size_t>(width);

    // Pre-allocate per-color sixel row buffers (reused across bands)
    auto colorRows = std::vector<std::vector<std::uint8_t>>(static_cast<std::size_t>(paletteSize),
                                                            std::vector<std::uint8_t>(widthU, 0));
    auto activeColors = std::vector<int> {};
    activeColors.reserve(static_cast<std::size_t>(paletteSize));

    // Six pixel rows to a band. Counted in bands rather than stepped in sixes, because
    // std::views::stride is C++23 and not in every standard library core-cpp builds with
    // (Apple's libc++ has it in neither 16 nor 17).
    for (auto const band: std::views::iota(0, (height + 5) / 6))
    {
        auto const bandY = band * 6;
        // Pass 1: Accumulate sixel bits per color (single pass over pixels)
        auto const bandHeight = std::min(6, height - bandY);
        for (auto const bit: std::views::iota(0, bandHeight))
        {
            auto const y = bandY + bit;
            auto const rowOffset = static_cast<std::size_t>(y) * widthU;
            auto const bitMask = static_cast<std::uint8_t>(1 << bit);
            for (auto const x: std::views::iota(0uz, widthU))
            {
                auto const colorIdx = indexed[rowOffset + x];
                if (colorIdx >= 0)
                    colorRows[static_cast<std::size_t>(colorIdx)][x] |= bitMask;
            }
        }

        // Collect active colors for this band
        activeColors.clear();
        for (auto const colorIdx: std::views::iota(0, paletteSize))
        {
            auto const& row = colorRows[static_cast<std::size_t>(colorIdx)];
            auto isActive = false;
            for (auto const x: std::views::iota(0uz, widthU))
            {
                if (row[x] != 0)
                {
                    isActive = true;
                    break;
                }
            }
            if (isActive)
                activeColors.push_back(colorIdx);
        }

        // Pass 2: Emit sixel data with RLE (only active colors)
        for (auto const colorIdx: activeColors)
        {
            result += std::format("#{}", colorIdx);
            emitSixelRowRLE(result, colorRows[static_cast<std::size_t>(colorIdx)]);
            result += '$'; // Carriage return (go to beginning of same sixel band)
        }
        result += '-'; // New line (advance to next sixel band)

        // Clear only the active color buffers for next band
        for (auto const colorIdx: activeColors)
            std::fill(colorRows[static_cast<std::size_t>(colorIdx)].begin(),
                      colorRows[static_cast<std::size_t>(colorIdx)].end(),
                      std::uint8_t { 0 });
    }

    return result;
}

} // namespace core::tui
