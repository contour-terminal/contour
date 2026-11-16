// SPDX-License-Identifier: Apache-2.0
//
// The Sixel encoder's bytes are a function of the image alone: of which pixels it holds, never of
// the order a sort happened to leave equal ones in, and so never of the standard library.

#include <core/tui/Sixel.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <ranges>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

using core::tui::encodeSixel;
using core::tui::ImageData;

namespace
{

constexpr auto Width = 48;
constexpr auto Height = 48;
constexpr auto PaletteSize = 16;

/// FNV-1a, 64 bit. Not `std::hash`: its value differs between the standard libraries this file
/// exists to compare, so a constant recorded from it would be one library's constant.
[[nodiscard]] constexpr auto fnv1a64(std::string_view bytes) noexcept -> std::uint64_t
{
    auto hash = std::uint64_t { 0xcbf29ce484222325ULL };
    for (auto const ch: bytes)
    {
        hash ^= static_cast<std::uint8_t>(ch);
        hash *= std::uint64_t { 0x100000001b3ULL };
    }
    return hash;
}

/// One opaque pixel of the fixture, by position.
struct Rgb
{
    std::uint8_t r;
    std::uint8_t g;
    std::uint8_t b;
};

/// Every channel takes one of sixteen levels, scattered by a multiplicative hash of the position, so the
/// channel the quantizer sorts on is shared by many pixels that differ in the other two, in uneven
/// numbers -- the shape on which a median falls inside a run of equal keys and a sort by that channel
/// alone leaves the split to the sort's tie order.
[[nodiscard]] constexpr auto fixturePixel(int index) noexcept -> Rgb
{
    auto const hash = static_cast<std::uint32_t>(index) * std::uint32_t { 2654435761U };
    auto const level = [hash](int shift) {
        return static_cast<std::uint8_t>(((hash >> shift) % 16U) * 17U);
    };
    return Rgb { .r = level(8), .g = level(16), .b = level(24) };
}

/// @param order Which fixture pixel sits at each position.
/// @return The RGBA bytes of a Width x Height image laying the fixture's pixels out in @p order.
[[nodiscard]] auto rgbaInOrder(std::vector<int> const& order) -> std::vector<std::uint8_t>
{
    auto rgba = std::vector<std::uint8_t> {};
    rgba.reserve(order.size() * 4);
    for (auto const index: order)
    {
        auto const pixel = fixturePixel(index);
        rgba.insert(rgba.end(), { pixel.r, pixel.g, pixel.b, std::uint8_t { 255 } });
    }
    return rgba;
}

/// @return Every fixture index, in position order.
[[nodiscard]] auto identityOrder() -> std::vector<int>
{
    auto order = std::vector<int> {};
    for (auto const i: std::views::iota(0, Width * Height))
        order.push_back(i);
    return order;
}

/// @return The palette definitions (`#i;2;r;g;b`) of a Sixel string, in the order they were emitted.
[[nodiscard]] auto paletteOf(std::string const& sixel) -> std::vector<std::string>
{
    static auto const definition = std::regex { "#[0-9]+;2;[0-9]+;[0-9]+;[0-9]+" };
    auto palette = std::vector<std::string> {};
    for (auto const& match: std::ranges::subrange(
             std::sregex_iterator { sixel.begin(), sixel.end(), definition }, std::sregex_iterator {}))
        palette.push_back(match.str());
    return palette;
}

} // namespace

TEST_CASE("The Sixel fixture is the shape a tie-order dependency shows on", "[Sixel]")
{
    // Guards the two cases below against a fixture edited into one with no ties to order, on which
    // both would pass under the defect they exist for.
    auto reds = std::set<std::uint8_t> {};
    auto colours = std::set<std::tuple<std::uint8_t, std::uint8_t, std::uint8_t>> {};
    for (auto const i: std::views::iota(0, Width * Height))
    {
        auto const pixel = fixturePixel(i);
        reds.insert(pixel.r);
        colours.insert({ pixel.r, pixel.g, pixel.b });
    }
    // Far fewer levels than pixels, and far more colours than levels: each level is shared by pixels
    // that differ in the other channels.
    CHECK(reds.size() == 16);
    CHECK(colours.size() > 16 * reds.size());
}

TEST_CASE("The Sixel palette depends on which pixels an image holds, not on where they are", "[Sixel]")
{
    // Median cut reads a bucket as a set of colours, so laying the same pixels out in another order
    // must quantize to the same palette. A sort on the widest channel alone leaves pixels equal in
    // that channel in an order that follows the input, the median split then puts different ones in
    // each half, and the palette moves -- on every standard library, which is why this case can fail
    // wherever it runs rather than only where two libraries are compared.
    auto const forward = identityOrder();
    auto reversed = forward;
    std::ranges::reverse(reversed);

    auto const forwardRgba = rgbaInOrder(forward);
    auto const reversedRgba = rgbaInOrder(reversed);
    auto const forwardSixel =
        encodeSixel(ImageData { .pixels = forwardRgba, .width = Width, .height = Height }, PaletteSize);
    auto const reversedSixel =
        encodeSixel(ImageData { .pixels = reversedRgba, .width = Width, .height = Height }, PaletteSize);
    REQUIRE(forwardSixel.has_value());
    REQUIRE(reversedSixel.has_value());

    auto const forwardPalette = paletteOf(*forwardSixel);
    REQUIRE(forwardPalette.size() == PaletteSize);
    CHECK(paletteOf(*reversedSixel) == forwardPalette);
}

TEST_CASE("A Sixel encoding is the same bytes on every standard library", "[Sixel]")
{
    // Recorded once and compared everywhere: libstdc++, libc++ and MSVC's library each ran the
    // unstable sort to a different tie order, so before the fix each produced its own bytes here.
    constexpr auto RecordedDigest = std::uint64_t { 0xbace46a6344de1d9ULL };

    auto const rgba = rgbaInOrder(identityOrder());
    auto const sixel =
        encodeSixel(ImageData { .pixels = rgba, .width = Width, .height = Height }, PaletteSize);
    REQUIRE(sixel.has_value());
    CHECK(fnv1a64(*sixel) == RecordedDigest);
}
