// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/vt/GlyphGlossary.hpp>

#include <ranges>
#include <utility>

namespace vtbackend
{

namespace
{
    /// Tags registered glyph ids apart from every other tile keyed in the atlas ('GLYF').
    constexpr auto IdTag = uint32_t { 0x474C'5946 };

    /// @return A hash of everything that decides what the glyph's tiles look like.
    [[nodiscard]] crispy::StrongHash hashGlyph(GlyphLayout const& layout,
                                               GlyphOutline const& outline) noexcept
    {
        auto hash =
            crispy::StrongHash { IdTag,
                                 layout.unitsPerEm,
                                 (uint32_t { layout.advanceWidth.value_or(0) } << 16)
                                     | layout.lineHeight.value_or(0),
                                 static_cast<uint32_t>(
                                     // One past the widest int16, so "absent" is not a value.
                                     layout.extentBottom ? int32_t { *layout.extentBottom } : 0x10000) };
        hash = hash
               * crispy::StrongHash { static_cast<uint32_t>(layout.size),
                                      static_cast<uint32_t>(layout.horizontal),
                                      static_cast<uint32_t>(layout.vertical),
                                      static_cast<uint32_t>(layout.span) };
        hash = hash * crispy::StrongHash::compute(&layout.padding, sizeof(layout.padding));
        // Field by field: a point has padding bytes, which hold nothing in particular.
        for (auto const& point: outline.points)
            hash = hash
                   * crispy::StrongHash { static_cast<uint32_t>(point.x),
                                          static_cast<uint32_t>(point.y),
                                          static_cast<uint32_t>(point.kind),
                                          0 };
        return hash
               * crispy::StrongHash::compute(outline.contourEnds.data(),
                                             outline.contourEnds.size() * sizeof(uint16_t));
    }

    /// Composes the reply to each kind of request, carrying it out on the way.
    struct Executor
    {
        GlyphGlossary& glossary;
        SystemGlyphCoverage const& coverage;

        [[nodiscard]] std::string operator()(GlyphSupportRequest const& /*request*/) const
        {
            return glyph_reply::support();
        }

        [[nodiscard]] std::string operator()(GlyphCodepointQuery const& query) const
        {
            return glyph_reply::query(query.codepoint,
                                      GlyphSources { .system = coverage.systemCoverage(query.codepoint),
                                                     .glossary = glossary.find(query.codepoint)
                                                                     ? GlyphSources::Glossary::Present
                                                                     : GlyphSources::Glossary::Absent });
        }

        [[nodiscard]] std::string operator()(GlyphGraphemeQuery const& query) const
        {
            // Not listed in the `s` reply's `cap`, so a conforming client never sends it.
            return glyph_reply::graphemeUnsupported(query.codepoints);
        }

        [[nodiscard]] std::string operator()(GlyphRegistration&& registration) const
        {
            glossary.registerGlyph(
                registration.codepoint, registration.layout, std::move(registration.outline));
            return registration.reply == GlyphReplyMode::All ? glyph_reply::registered(registration.codepoint)
                                                             : std::string {};
        }

        [[nodiscard]] std::string operator()(GlyphClearRequest const& request) const
        {
            if (request.codepoint)
                glossary.clear(*request.codepoint);
            else
                glossary.clear();
            return request.reply == GlyphReplyMode::All ? glyph_reply::cleared(request.codepoint)
                                                        : std::string {};
        }
    };

    /// Composes the reply to a message that is not a request.
    struct Refuser
    {
        [[nodiscard]] std::string operator()(GlyphMessageIgnored const& /*error*/) const { return {}; }

        [[nodiscard]] std::string operator()(GlyphRegistrationRefused const& refused) const
        {
            return refused.reply == GlyphReplyMode::None
                       ? std::string {}
                       : glyph_reply::registrationRefused(refused.codepoint, refused.reason);
        }

        [[nodiscard]] std::string operator()(GlyphClearRefused const& refused) const
        {
            return refused.reply == GlyphReplyMode::None ? std::string {}
                                                         : glyph_reply::clearRefused(refused.codepoint);
        }

        [[nodiscard]] std::string operator()(GlyphQueryMalformed const& /*error*/) const
        {
            return glyph_reply::queryMalformed();
        }
    };
} // namespace

void GlyphGlossary::registerGlyph(char32_t codepoint, GlyphLayout const& layout, GlyphOutline outline)
{
    auto const id = hashGlyph(layout, outline);
    put(codepoint,
        std::make_shared<RegisteredGlyph const>(
            RegisteredGlyph { .id = id, .layout = layout, .outline = std::move(outline) }));
}

void GlyphGlossary::clear(char32_t codepoint) noexcept
{
    put(codepoint, nullptr);
}

void GlyphGlossary::clear() noexcept
{
    // Clearing what is already clear changes nothing a frame could show.
    if (empty())
        return;
    ++_generation;
    for (auto const slot: std::views::iota(size_t { 0 }, GlyphRegistrationSlots))
        if (_glyphs[slot])
            _changedAt[slot] = _generation;
    _glyphs.fill(nullptr);
    _count = 0;
    _snapshot.reset();
}

std::vector<GlyphChange> GlyphGlossary::changesSince(uint64_t generation) const
{
    auto changes = std::vector<GlyphChange> {};
    for (auto const slot: std::views::iota(size_t { 0 }, GlyphRegistrationSlots))
        if (_changedAt[slot] > generation)
            changes.push_back(GlyphChange { .slot = static_cast<uint16_t>(slot), .glyph = _glyphs[slot] });
    return changes;
}

std::shared_ptr<RegisteredGlyph const> GlyphGlossary::find(char32_t codepoint) const noexcept
{
    if (!isGlyphRegistrationCodepoint(codepoint))
        return nullptr;
    return _glyphs[codepoint - FirstGlyphRegistrationCodepoint];
}

std::shared_ptr<RegisteredGlyphs const> GlyphGlossary::snapshot() const
{
    if (empty())
        return nullptr;
    if (!_snapshot)
        _snapshot = std::make_shared<RegisteredGlyphs const>(_glyphs);
    return _snapshot;
}

void GlyphGlossary::put(char32_t codepoint, std::shared_ptr<RegisteredGlyph const> glyph) noexcept
{
    if (!isGlyphRegistrationCodepoint(codepoint))
        return;
    auto& slot = _glyphs[codepoint - FirstGlyphRegistrationCodepoint];
    if (!slot && !glyph)
        return;
    _count += (glyph ? 1U : 0U);
    _count -= (slot ? 1U : 0U);
    slot = std::move(glyph);
    _changedAt[codepoint - FirstGlyphRegistrationCodepoint] = ++_generation;
    _snapshot.reset();
}

std::string executeGlyphProtocol(GlyphGlossary& glossary,
                                 SystemGlyphCoverage const& coverage,
                                 std::string_view body)
{
    auto command = parseGlyphCommand(body);
    if (!command)
        return std::visit(Refuser {}, command.error());
    // Moved from: a registration hands its outline to the glossary instead of copying it.
    return std::visit(Executor { .glossary = glossary, .coverage = coverage }, std::move(*command));
}

} // namespace vtbackend
