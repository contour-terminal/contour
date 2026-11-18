// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/vt/GlyphProtocol.hpp>

#include <crispy/StrongHash.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace vtbackend
{

/// One glyph an application registered: what it draws and how it is placed in its cell.
struct RegisteredGlyph
{
    /// Content hash of the definition; the atlas key. A registration that replaces another with different
    /// content has a different id, so a tile rasterized for the old one is never served.
    crispy::StrongHash id {};
    GlyphDefinition definition {};
};

/// The glyphs of a glossary by slot (codepoint minus FirstGlyphRegistrationCodepoint); null = none.
using RegisteredGlyphs = std::array<std::shared_ptr<RegisteredGlyph const>, GlyphRegistrationSlots>;

/// @param glyphs A frame's registered glyphs; null when the session has none.
/// @param codepoints The text of one cell.
/// @return The glyph that cell draws in place of its text, or null.
[[nodiscard]] inline RegisteredGlyph const* registeredGlyphOf(RegisteredGlyphs const* glyphs,
                                                              std::u32string_view codepoints) noexcept
{
    // A registered glyph is a single codepoint and never joins with its neighbours.
    if (glyphs == nullptr || codepoints.size() != 1 || !isGlyphRegistrationCodepoint(codepoints.front()))
        return nullptr;
    return (*glyphs)[codepoints.front() - FirstGlyphRegistrationCodepoint].get();
}

/// Answers whether the fonts in use draw a codepoint: what a `q` reply reports as `system`.
class SystemGlyphCoverage
{
  public:
    virtual ~SystemGlyphCoverage() = default;

    /// @param codepoint A Unicode scalar value.
    /// @return Whether some font in the fallback chain has a glyph for @p codepoint.
    [[nodiscard]] virtual GlyphSources::System systemCoverage(char32_t codepoint) const = 0;
};

/// A SystemGlyphCoverage that knows of no font: every codepoint is absent.
class NoSystemGlyphCoverage final: public SystemGlyphCoverage
{
  public:
    [[nodiscard]] GlyphSources::System systemCoverage(char32_t /*codepoint*/) const override
    {
        return GlyphSources::System::Absent;
    }
};

/// What became of one slot of a glossary: the glyph now registered there, or that it was cleared.
struct GlyphChange
{
    uint16_t slot = 0;                            ///< The codepoint minus FirstGlyphRegistrationCodepoint.
    std::shared_ptr<RegisteredGlyph const> glyph; ///< The glyph registered there; null for a cleared slot.
};

/// The glyphs one terminal session has registered through the Glyph Protocol, one slot per codepoint of
/// the registration block.
///
/// Glyphs are immutable once registered: a registration makes a fresh RegisteredGlyph, so pointers held
/// elsewhere (render buffers) stay valid and unchanged.
class GlyphGlossary
{
  public:
    /// Registers a glyph at @p codepoint, replacing what was there.
    /// @param codepoint A codepoint of the registration block.
    /// @param definition The glyph.
    void registerGlyph(char32_t codepoint, GlyphDefinition definition);

    /// Clears the registration at @p codepoint, if there is one.
    /// @param codepoint A codepoint of the registration block.
    void clear(char32_t codepoint);

    /// Clears every registration (the `c` verb without a codepoint, and RIS).
    void clear() noexcept;

    /// @return Whether no glyph is registered.
    [[nodiscard]] bool empty() const noexcept { return _count == 0; }

    /// @param codepoint Any codepoint.
    /// @return The glyph registered at @p codepoint, or null.
    [[nodiscard]] std::shared_ptr<RegisteredGlyph const> find(char32_t codepoint) const noexcept;

    /// @return A counter that increases on every registration and every clear.
    [[nodiscard]] uint64_t generation() const noexcept { return _generation; }

    /// What changed since an earlier state of the glossary, for whoever keeps a copy of it elsewhere (the
    /// daemon, for the clients mirroring a session).
    /// @param generation A value generation() returned earlier; 0 for the state before anything changed.
    /// @return Every slot registered, replaced or cleared since then, ascending, as it stands now.
    [[nodiscard]] std::vector<GlyphChange> changesSince(uint64_t generation) const;

    /// @return The registered glyphs as a frame holds them: unchanged by later registrations. Null when
    ///         nothing is registered, so a frame of a session that never used the protocol pays nothing.
    [[nodiscard]] std::shared_ptr<RegisteredGlyphs const> snapshot() const;

  private:
    void put(char32_t codepoint, std::shared_ptr<RegisteredGlyph const> glyph);

    /// What the slots hold. Apart from the glossary itself, so a session that never uses the protocol --
    /// nearly all of them -- carries a pointer and not twenty-four kilobytes.
    struct Slots
    {
        RegisteredGlyphs glyphs {};
        /// Per slot, the generation its last change made; 0 for a slot never touched. @see changesSince
        std::array<uint64_t, GlyphRegistrationSlots> changedAt {};
    };

    std::unique_ptr<Slots> _slots; ///< Null until the first registration.
    size_t _count = 0;
    uint64_t _generation = 0;
    /// What snapshot() last returned, dropped by every change: a run of registrations between two frames
    /// costs one copy, not one each.
    mutable std::shared_ptr<RegisteredGlyphs const> _snapshot;
};

/// Carries out one Glyph Protocol message against a session's glossary.
/// @param glossary The session's glossary.
/// @param coverage What the fonts in use draw, for `q`.
/// @param body The APC body, starting with `25a1;` (@see isGlyphProtocol).
/// @return The reply to send to the application, a complete APC sequence; empty when the message asks for
///         none, or cannot be answered.
[[nodiscard]] std::string executeGlyphProtocol(GlyphGlossary& glossary,
                                               SystemGlyphCoverage const& coverage,
                                               std::string_view body);

} // namespace vtbackend
