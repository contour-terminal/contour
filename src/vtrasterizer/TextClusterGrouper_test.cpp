// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/test_helpers.h>

#include <vtrasterizer/TextClusterGrouper.h>

#include <crispy/escape.h>
#include <crispy/utils.h>

#include <libunicode/convert.h>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include <gsl/span>
#include <gsl/span_ext>

#include <catch2/catch_test_macros.hpp>

#include <string_view>
#include <variant>

using namespace std::string_view_literals;

using namespace vtbackend;
using namespace vtrasterizer;

using std::vector;

namespace
{

template <typename T, std::size_t Extent, typename S = T>
std::vector<T> to_vector(gsl::span<S, Extent> span)
{
    auto result = std::vector<T> {};
    result.reserve(span.size());
    for (auto const& item: span)
        result.emplace_back(item);
    return result;
}

struct TextClusterGroup
{
    std::u32string codepoints {};
    std::vector<int> clusters {};
    vtbackend::CellLocation initialPenPosition {};
    TextStyle style {};
    vtbackend::RGBColor color {};

    auto operator<=>(TextClusterGroup const&) const = default;
};

struct BoxDrawingCell
{
    vtbackend::CellLocation position {};
    char32_t codepoint {};
    vtbackend::RGBColor foregroundColor {};

    auto operator<=>(BoxDrawingCell const&) const = default;
};

using Event = std::variant<TextClusterGroup, BoxDrawingCell>;

struct FrameWriter
{
    TextClusterGrouper& grouper;
    CellLocation penPosition;

    constexpr FrameWriter(TextClusterGrouper& grouper, CellLocation penPosition = {}) noexcept:
        grouper(grouper), penPosition(penPosition)
    {
        grouper.beginFrame();
    }

    ~FrameWriter() { grouper.endFrame(); }

    FrameWriter& write(std::u32string_view textCluster, TextStyle style, vtbackend::RGBColor color)
    {
        for (auto const codepoint: textCluster)
        {
            grouper.renderCell(penPosition, std::u32string_view(&codepoint, 1), style, color);
            ++penPosition.column;
        }
        return *this;
    }
};

} // namespace

// {{{ fmt::formatter for cuswtom times
template <>
struct fmt::formatter<TextClusterGroup>: formatter<std::string>
{
    template <typename FormatContext>
    auto format(TextClusterGroup const& group, FormatContext& ctx)
    {
        return formatter<std::string>::format(
            fmt::format("TextClusterGroup {{ codepoints: \"{}\", @{}, clusters={}, style: {}, color: {} }}",
                        crispy::escape(unicode::convert_to<char>(std::u32string_view(group.codepoints))),
                        group.initialPenPosition,
                        group.clusters,
                        group.style,
                        group.color),
            ctx);
    }
};

template <>
struct fmt::formatter<BoxDrawingCell>: formatter<std::string>
{
    template <typename FormatContext>
    auto format(BoxDrawingCell const& cell, FormatContext& ctx)
    {
        return formatter<std::string>::format(
            fmt::format("BoxDrawingCell {{ position: {}, codepoint: U+{:04X}, color: {} }}",
                        cell.position,
                        (unsigned) cell.codepoint,
                        cell.foregroundColor),
            ctx);
    }
};

template <>
struct fmt::formatter<Event>: formatter<std::string>
{
    template <typename FormatContext>
    auto format(Event const& event, FormatContext& ctx)
    {
        if (std::holds_alternative<TextClusterGroup>(event))
            return formatter<std::string>::format(fmt::format("{}", std::get<TextClusterGroup>(event)), ctx);
        else
            return formatter<std::string>::format(fmt::format("{}", std::get<BoxDrawingCell>(event)), ctx);
    }
};
// }}}

// {{{ EventRecorder
namespace
{
struct EventRecorder final: public TextClusterGrouper::Events
{
    std::vector<Event> events;

    void renderTextGroup(std::u32string_view codepoints,
                         gsl::span<unsigned> clusters,
                         vtbackend::CellLocation initialPenPosition,
                         TextStyle style,
                         vtbackend::RGBColor color) override
    {
        events.emplace_back(TextClusterGroup {
            std::u32string(codepoints), to_vector<int>(clusters), initialPenPosition, style, color });
    }

    bool renderBoxDrawingCell(vtbackend::CellLocation position,
                              char32_t codepoint,
                              vtbackend::RGBColor foregroundColor) override
    {
        events.emplace_back(BoxDrawingCell { position, codepoint, foregroundColor });
        return true;
    }
};
} // namespace
// }}}

// {{{ ostream <<
namespace std
{
ostream& operator<<(std::ostream& os, u32string_view text)
{
    return os << '"' << crispy::escape(unicode::convert_to<char>(text)) << '"';
}

ostream& operator<<(std::ostream& os, TextClusterGroup const& group)
{
    return os << fmt::format("{}", group);
}
} // namespace std
// }}}

TEST_CASE("TextClusterGrouper.renderLine")
{
    // Here, we want to make sure that the text grouping does not
    // include the whitespaces, also if it's more than a single whitespace,
    // All the other callback parameters must have been passed correctly.

    auto recorder = EventRecorder {};
    auto grouper = TextClusterGrouper(recorder);

    grouper.beginFrame();
    grouper.renderLine("Hello, World!", LineOffset(0), RGBColor { 0xF0, 0x80, 0x40 }, TextStyle::Regular);
    grouper.endFrame();

    REQUIRE(recorder.events.size() == 2);

    REQUIRE(std::holds_alternative<TextClusterGroup>(recorder.events[0]));
    auto const& clusterGroup = std::get<TextClusterGroup>(recorder.events[0]);
    CHECK(clusterGroup.codepoints == U"Hello,"sv); // FIXME should not contain space
    CHECK(clusterGroup.clusters == std::vector { 0, 1, 2, 3, 4, 5 });
    CHECK(clusterGroup.initialPenPosition == CellLocation {});
    CHECK(clusterGroup.style == TextStyle::Regular);
    CHECK(clusterGroup.color == RGBColor { 0xF0, 0x80, 0x40 });

    REQUIRE(std::holds_alternative<TextClusterGroup>(recorder.events[1]));
    auto const& clusterGroup2 = std::get<TextClusterGroup>(recorder.events[1]);
    CHECK(clusterGroup2.codepoints == U"World!"sv);
    CHECK(clusterGroup2.clusters == std::vector { 0, 1, 2, 3, 4, 5 });
    CHECK(clusterGroup2.initialPenPosition == CellLocation { LineOffset(0), ColumnOffset(7) });
    CHECK(clusterGroup2.style == TextStyle::Regular);
    CHECK(clusterGroup2.color == RGBColor { 0xF0, 0x80, 0x40 });
}

TEST_CASE("TextClusterGrouper.renderLine.DoubleWhitespace")
{
    // Here, we want to make sure that the text grouping does not
    // include the whitespaces, also if it's more than a single whitespace,
    // and that the initial pen position is advanced accordingly.

    auto recorder = EventRecorder {};
    auto grouper = TextClusterGrouper(recorder);

    grouper.beginFrame();
    grouper.renderLine("Hello,  World!", LineOffset(0), RGBColor { 0xF0, 0x80, 0x40 }, TextStyle::Regular);
    grouper.endFrame();

    REQUIRE(recorder.events.size() == 2);

    REQUIRE(std::holds_alternative<TextClusterGroup>(recorder.events[0]));
    auto const& clusterGroup = std::get<TextClusterGroup>(recorder.events[0]);
    CHECK(clusterGroup.codepoints == U"Hello,"sv); // FIXME should not contain space
    CHECK(clusterGroup.clusters == std::vector { 0, 1, 2, 3, 4, 5 });
    CHECK(clusterGroup.initialPenPosition == CellLocation {});
    CHECK(clusterGroup.style == TextStyle::Regular);
    CHECK(clusterGroup.color == RGBColor { 0xF0, 0x80, 0x40 });

    REQUIRE(std::holds_alternative<TextClusterGroup>(recorder.events[1]));
    auto const& clusterGroup2 = std::get<TextClusterGroup>(recorder.events[1]);
    CHECK(clusterGroup2.codepoints == U"World!"sv);
    CHECK(clusterGroup2.clusters == std::vector { 0, 1, 2, 3, 4, 5 });
    CHECK(clusterGroup2.initialPenPosition == CellLocation { LineOffset(0), ColumnOffset(8) });
    CHECK(clusterGroup2.style == TextStyle::Regular);
    CHECK(clusterGroup2.color == RGBColor { 0xF0, 0x80, 0x40 });
}

TEST_CASE("TextClusterGrouper.SplitAtColorChange")
{
    auto recorder = EventRecorder {};
    auto grouper = TextClusterGrouper(recorder);

    FrameWriter(grouper)
        .write(U"template", TextStyle::Bold, 0x102030_rgb)
        .write(U"...", TextStyle::Bold, 0x405060_rgb);

    REQUIRE(recorder.events.size() == 2);

    CHECK(std::get<TextClusterGroup>(recorder.events[0])
          == TextClusterGroup { .codepoints = U"template",
                                .clusters = { 0, 1, 2, 3, 4, 5, 6, 7 },
                                .initialPenPosition = {},
                                .style = TextStyle::Bold,
                                .color = 0x102030_rgb });
    CHECK(std::get<TextClusterGroup>(recorder.events[1])
          == TextClusterGroup { .codepoints = U"...",
                                .clusters = { 0, 1, 2 },
                                .initialPenPosition = CellLocation { LineOffset(0), ColumnOffset(8) },
                                .style = TextStyle::Bold,
                                .color = 0x405060_rgb });
}
