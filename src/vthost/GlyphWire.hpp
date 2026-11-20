// SPDX-License-Identifier: Apache-2.0
#pragma once

/// The glyphs of a Glyph Protocol glossary as they travel on the native protocol
/// (proto::RegisteredGlyphState).
///
/// A glyph travels as the application stated it: its `vg` payload, its viewBox and its placement. The
/// mirror reads the payload with vtbackend's own parser, the one the daemon read it with, so what it holds
/// was judged by the Glyph Protocol's rules whatever the daemon sent.

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
    Layout,  ///< A viewBox or a placement the Glyph Protocol's `r` could not have produced.
    Payload, ///< A payload the Glyph Protocol's `r` would have refused.
};

/// One slot of a glossary as a mirror applies it: a glyph to register, or a slot to clear.
struct GlyphUpdate
{
    char32_t codepoint = 0;                               ///< In the registration block.
    std::optional<vtbackend::GlyphDefinition> definition; ///< Absent: the slot is cleared.
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

/// @param glyph One slot as it travels.
/// @return What it weighs in a push: its payload's bytes, and one for the slot.
[[nodiscard]] inline size_t wireGlyphWeight(proto::WireRegisteredGlyph const& glyph) noexcept
{
    return glyph.payload.size() + 1;
}

/// Judges one slot a peer sent.
///
/// A daemon is not trusted to send only what an application could have registered: the payload is read
/// again here, by the parser that reads an application's, and every limit it enforces is enforced again.
/// @param glyph The slot.
/// @return What a glossary takes for it, or why it is refused.
[[nodiscard]] std::expected<GlyphUpdate, GlyphWireError> stageGlyphUpdate(
    proto::WireRegisteredGlyph const& glyph);

/// Judges the glyphs a peer sent (@see stageGlyphUpdate). Nothing is applied by this; a refused set leaves
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
