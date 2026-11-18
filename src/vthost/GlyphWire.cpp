// SPDX-License-Identifier: Apache-2.0
#include <vthost/GlyphWire.hpp>

#include <algorithm>
#include <bit>
#include <ranges>
#include <utility>

namespace vthost
{

namespace
{
    /// @return @p value as @p Enum when it is at most @p last, the enumeration's greatest enumerator.
    template <typename Enum>
    [[nodiscard]] constexpr std::optional<Enum> enumerator(uint8_t value, Enum last) noexcept
    {
        return value <= std::to_underlying(last) ? std::optional { static_cast<Enum>(value) } : std::nullopt;
    }

    [[nodiscard]] std::optional<vtbackend::GlyphLayout> layoutFromWire(proto::WireRegisteredGlyph const& wire)
    {
        using namespace vtbackend;
        auto const fit = enumerator(wire.fit, GlyphFit::Stretch);
        auto const horizontal = enumerator(wire.horizontal, GlyphAlignment::End);
        auto const vertical = enumerator(wire.vertical, GlyphAlignment::End);
        if (!fit || !horizontal || !vertical
            || (wire.span != std::to_underlying(GlyphRenderSpan::OneCell)
                && wire.span != std::to_underlying(GlyphRenderSpan::TwoCells)))
            return std::nullopt;
        return GlyphLayout { .fit = *fit,
                             .horizontal = *horizontal,
                             .vertical = *vertical,
                             .span = static_cast<GlyphRenderSpan>(wire.span) };
    }
} // namespace

proto::WireRegisteredGlyph glyphToWire(vtbackend::GlyphChange const& change)
{
    auto wire = proto::WireRegisteredGlyph { .slot = change.slot };
    if (!change.glyph)
        return wire;

    auto const& definition = change.glyph->definition;
    auto const& viewBox = definition.glyph.viewBox;
    wire.registered = 1;
    wire.viewBox = { std::bit_cast<uint32_t>(viewBox.x),
                     std::bit_cast<uint32_t>(viewBox.y),
                     std::bit_cast<uint32_t>(viewBox.width),
                     std::bit_cast<uint32_t>(viewBox.height) };
    wire.fit = std::to_underlying(definition.layout.fit);
    wire.horizontal = std::to_underlying(definition.layout.horizontal);
    wire.vertical = std::to_underlying(definition.layout.vertical);
    wire.span = std::to_underlying(definition.layout.span);
    wire.payload = definition.payload;
    return wire;
}

std::vector<proto::WireRegisteredGlyph> glyphsToWire(std::span<vtbackend::GlyphChange const> changes)
{
    return std::ranges::to<std::vector>(changes | std::views::transform(glyphToWire));
}

std::expected<GlyphUpdate, GlyphWireError> stageGlyphUpdate(proto::WireRegisteredGlyph const& glyph)
{
    if (glyph.slot >= vtbackend::GlyphRegistrationSlots)
        return std::unexpected { GlyphWireError::Slot };
    auto update = GlyphUpdate { .codepoint = static_cast<char32_t>(vtbackend::FirstGlyphRegistrationCodepoint
                                                                   + glyph.slot),
                                .definition = std::nullopt };
    if (glyph.registered == 0)
        return update;

    auto const layout = layoutFromWire(glyph);
    auto const viewBox = vtbackend::VectorViewBox { .x = std::bit_cast<float>(glyph.viewBox[0]),
                                                    .y = std::bit_cast<float>(glyph.viewBox[1]),
                                                    .width = std::bit_cast<float>(glyph.viewBox[2]),
                                                    .height = std::bit_cast<float>(glyph.viewBox[3]) };
    if (!layout || !vtbackend::isValidVectorViewBox(viewBox))
        return std::unexpected { GlyphWireError::Layout };
    if (glyph.payload.size() > vtbackend::MaxGlyphPayloadBytes)
        return std::unexpected { GlyphWireError::Payload };
    auto parsed = vtbackend::parseVectorGlyph(glyph.payload, viewBox);
    if (!parsed)
        return std::unexpected { GlyphWireError::Payload };
    update.definition = vtbackend::GlyphDefinition { .layout = *layout,
                                                     .glyph = std::move(*parsed),
                                                     .payload = glyph.payload };
    return update;
}

std::expected<std::vector<GlyphUpdate>, GlyphWireError> stageGlyphUpdates(
    std::span<proto::WireRegisteredGlyph const> glyphs)
{
    auto updates = std::vector<GlyphUpdate> {};
    updates.reserve(glyphs.size());
    for (auto const& glyph: glyphs)
    {
        auto update = stageGlyphUpdate(glyph);
        if (!update)
            return std::unexpected { update.error() };
        updates.push_back(std::move(*update));
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
            glossary.registerGlyph(update.codepoint, std::move(*update.definition));
        else
            glossary.clear(update.codepoint);
    }
}

} // namespace vthost
