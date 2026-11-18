// SPDX-License-Identifier: Apache-2.0
#include <vthost/GlyphWire.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <ranges>
#include <utility>

namespace vthost
{

namespace
{
    // The bits of proto::WireRegisteredGlyph::extentFields.
    constexpr auto HasAdvanceWidth = uint8_t { 0x01 };
    constexpr auto HasLineHeight = uint8_t { 0x02 };
    constexpr auto HasExtentBottom = uint8_t { 0x04 };
    constexpr auto ExtentFieldsMask = uint8_t { HasAdvanceWidth | HasLineHeight | HasExtentBottom };

    /// The farthest from the origin a point may lie. A `glyf` record's points are sums of 16-bit deltas, so
    /// the most its 5,461 points can reach is some 1.8e8; nothing an application registered lies beyond.
    constexpr auto MaxCoordinate = int32_t { 1 } << 28;

    /// @return @p value as @p Enum when it is at most @p last, the enumeration's greatest enumerator.
    template <typename Enum>
    [[nodiscard]] constexpr std::optional<Enum> enumerator(uint8_t value, Enum last) noexcept
    {
        return value <= std::to_underlying(last) ? std::optional { static_cast<Enum>(value) } : std::nullopt;
    }

    [[nodiscard]] std::optional<vtbackend::GlyphPadding> paddingFromWire(std::array<uint32_t, 4> const& bits)
    {
        auto const fraction = [](uint32_t value) {
            return std::bit_cast<float>(value);
        };
        auto const padding = vtbackend::GlyphPadding { .top = fraction(bits[0]),
                                                       .right = fraction(bits[1]),
                                                       .bottom = fraction(bits[2]),
                                                       .left = fraction(bits[3]) };
        // What the `pad=` parser leaves: fractions of the span, never the whole of it. The negated
        // comparison refuses a NaN too.
        auto const inRange = [](float value) {
            return value >= 0.0F && value <= 1.0F;
        };
        if (!inRange(padding.top) || !inRange(padding.right) || !inRange(padding.bottom)
            || !inRange(padding.left) || !(padding.left + padding.right < 1.0F)
            || !(padding.top + padding.bottom < 1.0F))
            return std::nullopt;
        return padding;
    }

    [[nodiscard]] std::optional<vtbackend::GlyphLayout> layoutFromWire(proto::WireRegisteredGlyph const& wire)
    {
        using namespace vtbackend;
        auto const size = enumerator(wire.size, GlyphSizeMode::Stretch);
        auto const horizontal = enumerator(wire.horizontal, GlyphHorizontalAlignment::End);
        auto const vertical = enumerator(wire.vertical, GlyphVerticalAlignment::Baseline);
        auto const padding = paddingFromWire(wire.padding);
        auto const declares = [&](uint8_t field) {
            return (wire.extentFields & field) != 0;
        };
        if (!size || !horizontal || !vertical || !padding || wire.unitsPerEm == 0
            || (wire.extentFields & ~ExtentFieldsMask) != 0
            || (wire.span != std::to_underlying(GlyphRenderSpan::OneCell)
                && wire.span != std::to_underlying(GlyphRenderSpan::TwoCells))
            || (declares(HasAdvanceWidth) && wire.advanceWidth == 0)
            || (declares(HasLineHeight) && wire.lineHeight == 0))
            return std::nullopt;

        return GlyphLayout {
            .unitsPerEm = wire.unitsPerEm,
            .advanceWidth = declares(HasAdvanceWidth) ? std::optional { wire.advanceWidth } : std::nullopt,
            .lineHeight = declares(HasLineHeight) ? std::optional { wire.lineHeight } : std::nullopt,
            .extentBottom = declares(HasExtentBottom) ? std::optional { wire.extentBottom } : std::nullopt,
            .size = *size,
            .horizontal = *horizontal,
            .vertical = *vertical,
            .padding = *padding,
            .span = static_cast<GlyphRenderSpan>(wire.span),
        };
    }

    [[nodiscard]] std::optional<vtbackend::GlyphOutline> outlineFromWire(
        proto::WireRegisteredGlyph const& wire)
    {
        using namespace vtbackend;
        if (wire.points.size() > MaxGlyphOutlinePoints)
            return std::nullopt;
        // The contours partition the points: each ends after the one before, and the last ends the list.
        auto const increasing =
            std::ranges::adjacent_find(wire.contourEnds, std::greater_equal {}) == wire.contourEnds.end();
        auto const covered = wire.contourEnds.empty() ? size_t { 0 } : size_t { wire.contourEnds.back() } + 1;
        if (!increasing || covered != wire.points.size())
            return std::nullopt;

        auto outline = GlyphOutline { .points = {}, .contourEnds = wire.contourEnds, .bounds = {} };
        outline.points.reserve(wire.points.size());
        for (auto const& point: wire.points)
        {
            if (point.onCurve > 1 || std::abs(int64_t { point.x }) > MaxCoordinate
                || std::abs(int64_t { point.y }) > MaxCoordinate)
                return std::nullopt;
            outline.points.push_back(GlyphOutlinePoint {
                .x = point.x,
                .y = point.y,
                .kind = point.onCurve != 0 ? GlyphPointKind::OnCurve : GlyphPointKind::OffCurve });
        }
        outline.bounds = boundsOf(outline.points);
        return outline;
    }

    [[nodiscard]] proto::WireRegisteredGlyph glyphToWire(vtbackend::GlyphChange const& change)
    {
        auto wire = proto::WireRegisteredGlyph { .slot = change.slot };
        if (!change.glyph)
            return wire;

        auto const& layout = change.glyph->layout;
        auto const& outline = change.glyph->outline;
        wire.registered = 1;
        wire.unitsPerEm = layout.unitsPerEm;
        wire.extentFields = static_cast<uint8_t>((layout.advanceWidth ? HasAdvanceWidth : 0)
                                                 | (layout.lineHeight ? HasLineHeight : 0)
                                                 | (layout.extentBottom ? HasExtentBottom : 0));
        wire.advanceWidth = layout.advanceWidth.value_or(0);
        wire.lineHeight = layout.lineHeight.value_or(0);
        wire.extentBottom = layout.extentBottom.value_or(0);
        wire.size = std::to_underlying(layout.size);
        wire.horizontal = std::to_underlying(layout.horizontal);
        wire.vertical = std::to_underlying(layout.vertical);
        wire.span = std::to_underlying(layout.span);
        wire.padding = { std::bit_cast<uint32_t>(layout.padding.top),
                         std::bit_cast<uint32_t>(layout.padding.right),
                         std::bit_cast<uint32_t>(layout.padding.bottom),
                         std::bit_cast<uint32_t>(layout.padding.left) };
        wire.contourEnds = outline.contourEnds;
        wire.points.reserve(outline.points.size());
        for (auto const& point: outline.points)
            wire.points.push_back(proto::WireGlyphPoint {
                .x = point.x,
                .y = point.y,
                .onCurve =
                    point.kind == vtbackend::GlyphPointKind::OnCurve ? uint8_t { 1 } : uint8_t { 0 } });
        return wire;
    }
} // namespace

std::vector<proto::WireRegisteredGlyph> glyphsToWire(std::span<vtbackend::GlyphChange const> changes)
{
    return std::ranges::to<std::vector>(changes | std::views::transform(glyphToWire));
}

std::expected<std::vector<GlyphUpdate>, GlyphWireError> stageGlyphUpdates(
    std::span<proto::WireRegisteredGlyph const> glyphs)
{
    auto updates = std::vector<GlyphUpdate> {};
    updates.reserve(glyphs.size());
    for (auto const& wire: glyphs)
    {
        if (wire.slot >= vtbackend::GlyphRegistrationSlots)
            return std::unexpected { GlyphWireError::Slot };
        auto& update = updates.emplace_back(GlyphUpdate {
            .codepoint = static_cast<char32_t>(vtbackend::FirstGlyphRegistrationCodepoint + wire.slot),
            .definition = std::nullopt });
        if (wire.registered == 0)
            continue;

        auto layout = layoutFromWire(wire);
        if (!layout)
            return std::unexpected { GlyphWireError::Layout };
        auto outline = outlineFromWire(wire);
        if (!outline)
            return std::unexpected { GlyphWireError::Outline };
        update.definition = GlyphUpdate::Definition { .layout = *layout, .outline = std::move(*outline) };
    }
    return updates;
}

void applyGlyphUpdates(vtbackend::GlyphGlossary& glossary,
                       std::vector<GlyphUpdate> updates,
                       GlyphUpdateMode mode)
{
    if (mode == GlyphUpdateMode::Replace)
        glossary.clear();
    for (auto& update: updates)
    {
        if (update.definition)
            glossary.registerGlyph(
                update.codepoint, update.definition->layout, std::move(update.definition->outline));
        else
            glossary.clear(update.codepoint);
    }
}

} // namespace vthost
