// SPDX-License-Identifier: Apache-2.0
#include <contour/session/PastePreview.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

using contour::session::formatByteCount;
using contour::session::makePastePreview;
using contour::session::PastePreviewLimits;

namespace
{

/// The most bytes a shown line may hold: its limit, and the ellipsis a cut line ends with.
constexpr auto MaxShownLineBytes = PastePreviewLimits {}.bytesPerLine + std::string_view { "…" }.size();

/// Whether @p text is well-formed UTF-8: every lead byte followed by exactly its continuation bytes.
[[nodiscard]] bool isWellFormedUtf8(std::string_view text)
{
    auto pending = 0;
    for (auto const c: text)
    {
        auto const byte = static_cast<unsigned char>(c);
        if (pending > 0)
        {
            if ((byte & 0b1100'0000U) != 0b1000'0000U)
                return false;
            --pending;
        }
        else if ((byte & 0b1000'0000U) == 0)
            continue;
        else if ((byte & 0b1110'0000U) == 0b1100'0000U)
            pending = 1;
        else if ((byte & 0b1111'0000U) == 0b1110'0000U)
            pending = 2;
        else if ((byte & 0b1111'1000U) == 0b1111'0000U)
            pending = 3;
        else
            return false;
    }
    return pending == 0;
}

} // namespace

TEST_CASE("pastePreview.boundsLinesAndCountsTheRest", "[pastepreview]")
{
    constexpr auto LineCount = std::size_t { 48'213 };
    auto text = std::string {};
    for (auto const i: std::views::iota(std::size_t { 0 }, LineCount))
        text += std::format("line {}\n", i);

    auto const preview = makePastePreview(text);

    CHECK(preview.lines == std::vector<std::string> { "line 0", "line 1" });
    CHECK(preview.moreLines == LineCount - PastePreviewLimits {}.lines);
}

TEST_CASE("pastePreview.cutsALongLineAtACharacter", "[pastepreview]")
{
    constexpr auto Bytes = std::size_t { 600'000 };
    auto const e = std::string_view { "é" };
    auto text = std::string {};
    text.reserve(Bytes);
    while (text.size() < Bytes)
        text += e;

    auto const preview = makePastePreview(text);

    REQUIRE(preview.lines.size() == 1);
    auto const& line = preview.lines.front();
    CHECK(line.size() <= MaxShownLineBytes);
    CHECK(isWellFormedUtf8(line));
    CHECK(line.ends_with("…"));
    CHECK(preview.moreLines == 0);
}

TEST_CASE("pastePreview.showsControlsAsReplacement", "[pastepreview]")
{
    auto const preview = makePastePreview("\x1b[31mred\n");

    REQUIRE(preview.lines.size() == 1);
    CHECK_FALSE(preview.lines.front().contains('\x1b'));
    CHECK(preview.lines.front().contains("red"));
}

TEST_CASE("pastePreview.keepsItsBoundForALineOfControls", "[pastepreview]")
{
    // Sanitizing turns each control into a three-byte U+FFFD: cut before, the line would grow past its bound.
    constexpr auto Controls = std::size_t { 200 };
    auto const preview = makePastePreview(std::string(Controls, '\x01'));

    REQUIRE(preview.lines.size() == 1);
    CHECK(preview.lines.front().size() <= MaxShownLineBytes);
    CHECK(isWellFormedUtf8(preview.lines.front()));
    CHECK(preview.lines.front().ends_with("…"));
    CHECK_FALSE(preview.lines.front().contains('\x01'));
}

TEST_CASE("pastePreview.emptyAndTrailingNewline", "[pastepreview]")
{
    CHECK(makePastePreview("").lines.empty());
    CHECK(makePastePreview("").moreLines == 0);

    auto const preview = makePastePreview("a\n");
    CHECK(preview.lines == std::vector<std::string> { "a" });
    CHECK(preview.moreLines == 0);
}

TEST_CASE("formatByteCount", "[pastepreview]")
{
    CHECK(formatByteCount(1) == "1 byte");
    CHECK(formatByteCount(999) == "999 bytes");
    CHECK(formatByteCount(1000) == "1.0 kB");
    CHECK(formatByteCount(524'289) == "524.3 kB");
    CHECK(formatByteCount(2'400'000) == "2.4 MB");
    CHECK(formatByteCount(52'428'800) == "52.4 MB");
}
