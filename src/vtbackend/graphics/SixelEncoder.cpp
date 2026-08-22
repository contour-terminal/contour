// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/graphics/SixelEncoder.hpp>

#include <vtbackend/graphics/SixelParser.hpp>

#include <crispy/Assert.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <ranges>
#include <unordered_map>
#include <utility>
#include <vector>

using std::string;

namespace vtbackend
{

namespace
{
    /// Marks a pixel no palette entry covers, which is how a transparent one is carried through: the
    /// encoder simply never paints it, and the `P2` parameter tells the decoder to leave it alone.
    constexpr auto Unpainted = uint16_t { 0xFFFF };

    /// A run shorter than this costs more as `!<count><char>` than written out.
    constexpr auto ShortestWorthwhileRun = size_t { 4 };

    /// The fixed cut of RGB used when an image has more distinct colors than sixel has registers.
    ///
    /// A fixed cut rather than a median cut on purpose: it needs no per-image analysis, cannot
    /// overflow the palette, and the case it exists for -- a background image or inline graphics
    /// behind the text -- is exactly the case where a screenshot is not being read for its colors.
    /// Green gets the extra level because the eye resolves it best. 6 * 7 * 6 = 252 registers.
    constexpr auto RedLevels = unsigned { 6 };
    constexpr auto GreenLevels = unsigned { 7 };
    constexpr auto BlueLevels = unsigned { 6 };

    /// A pixel is painted when it is at least half opaque. @see encodeSixel.
    constexpr auto OpaqueEnough = uint8_t { 128 };

    /// Converts an 8-bit channel to the percentage sixel states a color in.
    ///
    /// Rounded rather than truncated, so a channel that is already a whole percentage (0, 51, 102,
    /// 153, 204, 255) comes back out of the decoder unchanged.
    [[nodiscard]] constexpr uint8_t toPercent(uint8_t channel) noexcept
    {
        return static_cast<uint8_t>(((static_cast<unsigned>(channel) * 100u) + 127u) / 255u);
    }

    /// One palette entry, held as the three percentages the wire carries.
    ///
    /// Deliberately NOT an RGBColor, whose channels are 0..255 levels: these are 0..100 percentages,
    /// and a type that says "color" while holding a different unit is how a conversion goes missing.
    struct PaletteColor
    {
        uint8_t red = 0;
        uint8_t green = 0;
        uint8_t blue = 0;
    };

    /// Colors are keyed in percent space rather than in 8-bit, so two colors the wire cannot tell
    /// apart occupy one register instead of two -- which is what keeps a truecolor-shaded screen
    /// inside the palette that would otherwise overflow it.
    [[nodiscard]] constexpr uint32_t keyOf(PaletteColor color) noexcept
    {
        return (static_cast<uint32_t>(color.red) << 16) | (static_cast<uint32_t>(color.green) << 8)
               | color.blue;
    }

    /// @return The bucket @p channel falls in, for a cut of @p levels even steps.
    [[nodiscard]] constexpr unsigned bucketOf(uint8_t channel, unsigned levels) noexcept
    {
        return std::min((static_cast<unsigned>(channel) * levels) / 256u, levels - 1);
    }

    /// @return The 8-bit channel value at the middle of bucket @p bucket, with the outermost buckets
    ///         reaching the full range so black stays black and white stays white.
    [[nodiscard]] constexpr uint8_t bucketColor(unsigned bucket, unsigned levels) noexcept
    {
        return static_cast<uint8_t>((bucket * 255u) / (levels - 1));
    }

    /// An image with every pixel replaced by the palette register that paints it.
    struct IndexedImage
    {
        std::vector<uint16_t> indices;
        std::vector<PaletteColor> palette;
    };

    /// Indexes @p rgba against the fixed cut described above, which always fits.
    [[nodiscard]] IndexedImage quantize(std::span<uint8_t const> rgba, size_t pixelCount)
    {
        auto result = IndexedImage {};
        result.indices.resize(pixelCount, Unpainted);
        result.palette.reserve(size_t { RedLevels } * GreenLevels * BlueLevels);

        for (auto const red: std::views::iota(0u, RedLevels))
            for (auto const green: std::views::iota(0u, GreenLevels))
                for (auto const blue: std::views::iota(0u, BlueLevels))
                    result.palette.push_back(
                        PaletteColor { .red = toPercent(bucketColor(red, RedLevels)),
                                       .green = toPercent(bucketColor(green, GreenLevels)),
                                       .blue = toPercent(bucketColor(blue, BlueLevels)) });

        for (auto const pixel: std::views::iota(size_t { 0 }, pixelCount))
        {
            auto const* const channels = &rgba[pixel * 4];
            if (channels[3] < OpaqueEnough)
                continue;

            // The same row-major walk the palette above was built in, so the index addresses it.
            auto const red = bucketOf(channels[0], RedLevels);
            auto const green = bucketOf(channels[1], GreenLevels);
            auto const blue = bucketOf(channels[2], BlueLevels);
            auto const bucket = (((red * GreenLevels) + green) * BlueLevels) + blue;
            result.indices[pixel] = static_cast<uint16_t>(bucket);
        }

        return result;
    }

    /// Assigns every pixel a palette register, exactly where the image has few enough distinct colors
    /// and by @ref quantize where it does not.
    [[nodiscard]] IndexedImage indexPixels(std::span<uint8_t const> rgba, size_t pixelCount)
    {
        auto result = IndexedImage {};
        result.indices.resize(pixelCount, Unpainted);
        auto registerOf = std::unordered_map<uint32_t, uint16_t> {};

        for (auto const pixel: std::views::iota(size_t { 0 }, pixelCount))
        {
            auto const* const channels = &rgba[pixel * 4];
            if (channels[3] < OpaqueEnough)
                continue;

            auto const color = PaletteColor { .red = toPercent(channels[0]),
                                              .green = toPercent(channels[1]),
                                              .blue = toPercent(channels[2]) };

            if (auto const known = registerOf.find(keyOf(color)); known != registerOf.end())
            {
                result.indices[pixel] = known->second;
                continue;
            }

            // One color past what the registers hold, and an exact palette is off the table for the
            // whole image -- so start over rather than carry a half-exact one.
            if (result.palette.size() == MaxSixelPaletteSize)
                return quantize(rgba, pixelCount);

            auto const assigned = static_cast<uint16_t>(result.palette.size());
            result.palette.push_back(color);
            registerOf.emplace(keyOf(color), assigned);
            result.indices[pixel] = assigned;
        }

        return result;
    }

    /// Appends @p value in decimal.
    ///
    /// Not std::format: this is reached once per run and once per register per band, which is close to
    /// a million calls for a page-sized frame, and each std::format pays a type-erased argument pack
    /// and a temporary string to emit at most a handful of characters. Measured at a third of the
    /// encoder's whole cost -- which lands on the RENDER thread, so it is dropped frames, not just
    /// microseconds.
    void appendDecimal(string& out, size_t value)
    {
        auto digits = std::array<char, 20> {};
        auto const written = std::to_chars(digits.data(), digits.data() + digits.size(), value);
        out.append(digits.data(), written.ptr);
    }

    /// Appends one color's row of sixel characters, run-length encoded.
    ///
    /// @param out   Receives the characters.
    /// @param masks One byte per column, each holding the six-bit pattern this color paints there.
    void appendRow(string& out, std::span<uint8_t const> masks)
    {
        // A run of unpainted columns at the end says nothing: whatever follows returns to the left
        // edge regardless, so transmitting it is pure cost.
        auto width = masks.size();
        while (width > 0 && masks[width - 1] == 0)
            --width;

        auto column = size_t { 0 };
        while (column < width)
        {
            auto const pattern = masks[column];
            auto run = size_t { 1 };
            while (column + run < width && masks[column + run] == pattern)
                ++run;
            column += run;

            auto const character = static_cast<char>(63 + pattern);
            if (run >= ShortestWorthwhileRun)
            {
                out += '!';
                appendDecimal(out, run);
                out += character;
            }
            else
                out.append(run, character);
        }
    }
} // namespace

string encodeSixel(std::span<uint8_t const> rgba, ImageSize size)
{
    auto const width = unbox<size_t>(size.width);
    auto const height = unbox<size_t>(size.height);
    Require(width > 0 && height > 0);
    Require(rgba.size() == width * height * 4);

    auto const image = indexPixels(rgba, width * height);

    // P1 = 7: square pixels. P2 = 1: a pixel this stream does not paint keeps whatever was there,
    // which is what leaving a transparent pixel out has to mean. P3 is unused by every reader.
    // The raster attribute restates the aspect and, more usefully, the extent -- so a decoder sizes
    // its canvas once instead of growing it band by band.
    auto out = std::format("\033P7;1;0q\"1;1;{};{}", width, height);
    out.reserve(out.size() + (width * ((height + SixelBitCount - 1) / SixelBitCount)) + 64);

    for (auto const entry: std::views::iota(size_t { 0 }, image.palette.size()))
    {
        auto const color = image.palette[entry];
        out += std::format("#{};2;{};{};{}", entry, color.red, color.green, color.blue);
    }

    // One column mask per palette register, refilled per band. Filling them in a single pass over the
    // band is what keeps this O(pixels) instead of O(pixels * registers).
    //
    // Which is also why they are cleared a lane at a time, as each is written out below, rather than
    // wholesale at the top of every band: a `fill` over the whole array is registers * width per band,
    // so a 256-color palette would cost more in memset than the encoding itself -- and this runs on the
    // RENDER thread. Only a register that painted something has a non-zero lane, and `painted` names
    // exactly those.
    auto masks = std::vector<uint8_t>(image.palette.size() * width, 0);
    auto painted = std::vector<uint8_t>(image.palette.size(), 0);

    for (auto bandTop = size_t { 0 }; bandTop < height; bandTop += SixelBitCount)
    {
        auto const bandRows = std::min(size_t { SixelBitCount }, height - bandTop);
        for (auto const row: std::views::iota(size_t { 0 }, bandRows))
            for (auto const column: std::views::iota(size_t { 0 }, width))
            {
                auto const index = image.indices[((bandTop + row) * width) + column];
                if (index == Unpainted)
                    continue;
                masks[(size_t { index } * width) + column] |= static_cast<uint8_t>(1u << row);
                painted[index] = 1;
            }

        auto first = true;
        for (auto const entry: std::views::iota(size_t { 0 }, image.palette.size()))
        {
            if (!painted[entry])
                continue;

            // Back to the left edge of the same band, to overlay the next color on it.
            if (!std::exchange(first, false))
                out += '$';

            out += '#';
            appendDecimal(out, entry);
            auto const lane = std::span { masks }.subspan(entry * width, width);
            appendRow(out, lane);

            // Written out, so it can go back to zero ready for the next band. @see masks.
            std::ranges::fill(lane, uint8_t { 0 });
            painted[entry] = 0;
        }

        if (bandTop + SixelBitCount < height)
            out += '-';
    }

    out += "\033\\";
    return out;
}

} // namespace vtbackend
