// SPDX-License-Identifier: Apache-2.0
#pragma once

/// The glyphs of a Glyph Protocol glossary as they travel on the native protocol
/// (proto::RegisteredGlyphState).
///
/// The wire carries a glyph as the glossary holds it, decoded: the layout's fields and the outline's points.
/// The mirror rebuilds it through vtbackend's own GlyphGlossary, which derives its atlas id and bounds.

#include <vtbackend/vt/GlyphGlossary.hpp>

#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <vector>

#include <vthost/proto/Pdu.hpp>

namespace vthost
{

/// Why a set of wire glyphs is not one a mirror takes.
enum class GlyphWireError : uint8_t
{
    Slot,    ///< A slot outside the registration block.
    Layout,  ///< A layout the Glyph Protocol's `r` could not have produced.
    Outline, ///< Contours that do not partition the points, a point of no kind, or too many of them.
};

/// One slot of a glossary as a mirror applies it: a glyph to register, or a slot to clear.
struct GlyphUpdate
{
    /// A glyph to register.
    struct Definition
    {
        vtbackend::GlyphLayout layout {};
        vtbackend::GlyphOutline outline {};
    };

    char32_t codepoint = 0;               ///< In the registration block.
    std::optional<Definition> definition; ///< Absent: the slot is cleared.
};

/// Captures one change of a glossary for the wire.
/// @param change A slot that changed (@see vtbackend::GlyphGlossary::changesSince).
/// @return The slot as it travels.
[[nodiscard]] proto::WireRegisteredGlyph glyphToWire(vtbackend::GlyphChange const& change);

/// Captures changes of a glossary for the wire.
/// @param changes The slots that changed.
/// @return One entry per change, in the same order.
[[nodiscard]] std::vector<proto::WireRegisteredGlyph> glyphsToWire(
    std::span<vtbackend::GlyphChange const> changes);

/// Judges one slot a peer sent (@see stageGlyphUpdates).
/// @param glyph The slot.
/// @return What a glossary takes for it, or why it is refused.
[[nodiscard]] std::expected<GlyphUpdate, GlyphWireError> stageGlyphUpdate(
    proto::WireRegisteredGlyph const& glyph);

/// @param glyph One slot as it travels.
/// @return How many outline points it carries, which is what makes a push large.
[[nodiscard]] constexpr size_t wireGlyphWeight(proto::WireRegisteredGlyph const& glyph) noexcept
{
    return glyph.points.size() + 1;
}

/// Judges the glyphs a peer sent, and turns them into what a glossary takes.
///
/// A daemon is not trusted to send only what an application could have registered: every limit the Glyph
/// Protocol's own parser enforces is enforced again here. Nothing is applied by this; a refused set leaves
/// the mirror's glossary as it was.
/// @param glyphs The slots a RegisteredGlyphState carried.
/// @return The updates, in the order sent, or why the set was refused.
[[nodiscard]] std::expected<std::vector<GlyphUpdate>, GlyphWireError> stageGlyphUpdates(
    std::span<proto::WireRegisteredGlyph const> glyphs);

/// Whether a set of updates replaces a glossary or changes it.
enum class GlyphUpdateMode : uint8_t
{
    Change,  ///< The slots named are changed; the rest stand.
    Replace, ///< Every slot is cleared first.
};

/// Applies staged updates to a glossary.
/// @param glossary The mirror's glossary (the caller holds its terminal's lock).
/// @param updates The updates, as stageGlyphUpdates() returned them. Consumed.
/// @param mode Whether the glossary is cleared first.
void applyGlyphUpdates(vtbackend::GlyphGlossary& glossary,
                       std::vector<GlyphUpdate> updates,
                       GlyphUpdateMode mode);

} // namespace vthost
