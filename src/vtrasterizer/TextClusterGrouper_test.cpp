// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/test_helpers.h>

#include <vtrasterizer/FontDescriptions.h>
#include <vtrasterizer/TextClusterGrouper.h>

#include <crispy/escape.h>
#include <crispy/utils.h>

#include <libunicode/convert.h>

#include <gsl/span>
#include <gsl/span_ext>

#include <catch2/catch_test_macros.hpp>

#include <format>
#include <ranges>
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
    vtbackend::RGBColor color {};
    TextStyle style {};
    vtbackend::LineFlags flags = vtbackend::LineFlag::None;

#ifdef __APPLE__
    // NB: Don't use default implementation for operator<=>,
    // as it's not yet supported by std::vector<> on macOS it seems.
    bool operator==(TextClusterGroup const& other) const noexcept
    {
        return codepoints == other.codepoints && clusters == other.clusters
               && initialPenPosition == other.initialPenPosition && style == other.style
               && color == other.color && flags == other.flags;
    }
#else
    auto operator<=>(TextClusterGroup const&) const = default;
#endif
};

struct BoxDrawingCell
{
    vtbackend::CellLocation position {};
    char32_t codepoint {};
    vtbackend::RGBColor foregroundColor {};
    vtbackend::LineFlags flags = vtbackend::LineFlag::None;

    auto operator<=>(BoxDrawingCell const&) const = default;
};

using Event = std::variant<TextClusterGroup, BoxDrawingCell>;

struct FrameWriter
{
    TextClusterGrouper& grouper;
    CellLocation penPosition;

    explicit FrameWriter(TextClusterGrouper& grouper, CellLocation penPosition = {}) noexcept:
        grouper(grouper), penPosition(penPosition)
    {
        grouper.beginFrame();
    }

    ~FrameWriter() { grouper.endFrame(); }

    FrameWriter& write(std::u32string_view textCluster,
                       vtbackend::RGBColor color,
                       TextStyle style,
                       vtbackend::LineFlags flags)
    {
        for (auto const codepoint: textCluster)
        {
            grouper.renderCell(penPosition, std::u32string_view(&codepoint, 1), color, style, flags);
            ++penPosition.column;
        }
        return *this;
    }
};

} // namespace

// {{{ std::formatter for cuswtom times
template <>
struct std::formatter<TextClusterGroup>: formatter<std::string>
{
    template <typename FormatContext>
    auto format(TextClusterGroup const& group, FormatContext& ctx) const
    {
        return formatter<std::string>::format(
            std::format("TextClusterGroup {{ codepoints: \"{}\", @{}, clusters={}, style: {}, color: {} }}",
                        crispy::escape(unicode::convert_to<char>(std::u32string_view(group.codepoints))),
                        group.initialPenPosition,
                        group.clusters | std::views::transform([](int cluster) {
                            return std::to_string(cluster);
                        }) | crispy::views::join_with(", "),
                        group.style,
                        group.color,
                        group.flags),
            ctx);
    }
};

template <>
struct std::formatter<BoxDrawingCell>: formatter<std::string>
{
    template <typename FormatContext>
    auto format(BoxDrawingCell const& cell, FormatContext& ctx) const
    {
        return formatter<std::string>::format(
            std::format("BoxDrawingCell {{ position: {}, codepoint: U+{:04X}, color: {} }}",
                        cell.position,
                        (unsigned) cell.codepoint,
                        cell.foregroundColor),
            ctx);
    }
};

template <>
struct std::formatter<Event>: formatter<std::string>
{
    template <typename FormatContext>
    auto format(Event const& event, FormatContext& ctx) const
    {
        if (std::holds_alternative<TextClusterGroup>(event))
            return formatter<std::string>::format(std::format("{}", std::get<TextClusterGroup>(event)), ctx);
        else
            return formatter<std::string>::format(std::format("{}", std::get<BoxDrawingCell>(event)), ctx);
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
                         vtbackend::RGBColor color,
                         vtbackend::LineFlags flags,
                         vtbackend::GlyphSizing const& sizing,
                         uint8_t bidiLevel) override
    {
        (void) sizing;
        (void) bidiLevel;
        events.emplace_back(TextClusterGroup {
            .codepoints = std::u32string(codepoints),
            .clusters = to_vector<int>(clusters),
            .initialPenPosition = initialPenPosition,
            .color = color,
            .style = style,
            .flags = flags,
        });
    }

    bool renderBoxDrawingCell(vtbackend::CellLocation position,
                              char32_t codepoint,
                              vtbackend::RGBColor foregroundColor,
                              vtbackend::LineFlags flags,
                              uint8_t bidiLevel) override
    {
        (void) bidiLevel;
        events.emplace_back(BoxDrawingCell {
            .position = position,
            .codepoint = codepoint,
            .foregroundColor = foregroundColor,
            .flags = flags,
        });
        return true;
    }
};
} // namespace
// }}}

// {{{ ostream <<
namespace std
{
static ostream& operator<<(std::ostream& os, u32string_view text)
{
    return os << '"' << crispy::escape(unicode::convert_to<char>(text)) << '"';
}

static ostream& operator<<(std::ostream& os, TextClusterGroup const& group)
{
    return os << std::format("{}", group);
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
    grouper.renderLine(
        U"Hello, World!", LineOffset(0), RGBColor { 0xF0, 0x80, 0x40 }, TextStyle::Regular, LineFlag::None);
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
    grouper.renderLine(
        U"Hello,  World!", LineOffset(0), RGBColor { 0xF0, 0x80, 0x40 }, TextStyle::Regular, LineFlag::None);
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
        .write(U"template", 0x102030_rgb, TextStyle::Bold, LineFlag::None)
        .write(U"...", 0x405060_rgb, TextStyle::Bold, LineFlag::None);

    REQUIRE(recorder.events.size() == 2);

    CHECK(std::get<TextClusterGroup>(recorder.events[0])
          == TextClusterGroup {
              .codepoints = U"template",
              .clusters = { 0, 1, 2, 3, 4, 5, 6, 7 },
              .initialPenPosition = {},
              .color = 0x102030_rgb,
              .style = TextStyle::Bold,
              .flags = vtbackend::LineFlag::None,
          });
    CHECK(std::get<TextClusterGroup>(recorder.events[1])
          == TextClusterGroup {
              .codepoints = U"...",
              .clusters = { 0, 1, 2 },
              .initialPenPosition = CellLocation { LineOffset(0), ColumnOffset(8) },
              .color = 0x405060_rgb,
              .style = TextStyle::Bold,
              .flags = vtbackend::LineFlag::None,
          });
}

// A shaping run may never straddle a change of writing direction: the shaper is told which way to
// lay the whole run out, so a group holding both directions would be laid out wrongly whichever
// answer it was given. A level change therefore ends a group exactly as a colour change does.
TEST_CASE("TextClusterGrouper.a level change ends the group")
{
    auto recorder = EventRecorder {};
    auto grouper = TextClusterGrouper(recorder);
    auto const color = RGBColor { 0xF0, 0x80, 0x40 };

    grouper.beginFrame();
    // Two cells at level 0, then two at level 1 -- no space, no colour change, nothing else that
    // would flush. Only the level differs.
    for (auto const [index, level]: std::array { std::pair { 0, uint8_t { 0 } },
                                                 std::pair { 1, uint8_t { 0 } },
                                                 std::pair { 2, uint8_t { 1 } },
                                                 std::pair { 3, uint8_t { 1 } } })
    {
        auto const codepoint = static_cast<char32_t>(U'a' + index);
        grouper.renderCell(CellLocation { .line = LineOffset(0), .column = ColumnOffset(index) },
                           std::u32string_view(&codepoint, 1),
                           color,
                           TextStyle::Regular,
                           LineFlag::None,
                           {},
                           level);
    }
    grouper.endFrame();

    REQUIRE(recorder.events.size() == 2);
    CHECK(get<TextClusterGroup>(recorder.events[0]).codepoints == U"ab");
    CHECK(get<TextClusterGroup>(recorder.events[1]).codepoints == U"cd");
}

TEST_CASE("TextClusterGrouper.a uniform level does not split")
{
    auto recorder = EventRecorder {};
    auto grouper = TextClusterGrouper(recorder);
    auto const color = RGBColor { 0xF0, 0x80, 0x40 };

    grouper.beginFrame();
    for (auto const index: { 0, 1, 2, 3 })
    {
        auto const codepoint = static_cast<char32_t>(U'a' + index);
        grouper.renderCell(CellLocation { .line = LineOffset(0), .column = ColumnOffset(index) },
                           std::u32string_view(&codepoint, 1),
                           color,
                           TextStyle::Regular,
                           LineFlag::None,
                           {},
                           uint8_t { 1 }); // all right-to-left
    }
    grouper.endFrame();

    // One run, not four: the level is part of the flush predicate, not a per-cell key.
    REQUIRE(recorder.events.size() == 1);
    CHECK(get<TextClusterGroup>(recorder.events[0]).codepoints == U"abcd");
}
