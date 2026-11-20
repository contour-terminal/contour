// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/vt/GlyphGlossary.hpp>

#include <bit>
#include <ranges>
#include <utility>

namespace vtbackend
{

namespace
{
    /// Tags registered glyph ids apart from every other tile keyed in the atlas ('GLVG').
    constexpr auto IdTag = uint32_t { 0x474C'5647 };

    /// @return A hash of everything that decides what the glyph's tiles look like: the payload, which is
    ///         the glyph, and where it is placed.
    [[nodiscard]] crispy::StrongHash hashGlyph(GlyphDefinition const& definition) noexcept
    {
        auto const& layout = definition.layout;
        auto const& viewBox = definition.glyph.viewBox;
        auto const bitsOf = [](float value) {
            return std::bit_cast<uint32_t>(value);
        };
        return crispy::StrongHash { IdTag,
                                    static_cast<uint32_t>(layout.fit),
                                    (static_cast<uint32_t>(layout.horizontal) << 8)
                                        | static_cast<uint32_t>(layout.vertical),
                                    static_cast<uint32_t>(layout.span) }
               * crispy::StrongHash { bitsOf(viewBox.x),
                                      bitsOf(viewBox.y),
                                      bitsOf(viewBox.width),
                                      bitsOf(viewBox.height) }
               * crispy::StrongHash::compute(definition.payload.data(), definition.payload.size());
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
            glossary.registerGlyph(registration.codepoint, std::move(registration.definition));
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

void GlyphGlossary::registerGlyph(char32_t codepoint, GlyphDefinition definition)
{
    auto const id = hashGlyph(definition);
    put(codepoint,
        std::make_shared<RegisteredGlyph const>(
            RegisteredGlyph { .id = id, .definition = std::move(definition) }));
}

void GlyphGlossary::clear(char32_t codepoint)
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
        if (_slots->glyphs[slot])
            _slots->changedAt[slot] = _generation;
    _slots->glyphs.fill(nullptr);
    _count = 0;
    _snapshot.reset();
}

std::vector<GlyphChange> GlyphGlossary::changesSince(uint64_t generation) const
{
    auto changes = std::vector<GlyphChange> {};
    if (!_slots)
        return changes;
    for (auto const slot: std::views::iota(size_t { 0 }, GlyphRegistrationSlots))
        if (_slots->changedAt[slot] > generation)
            changes.push_back(
                GlyphChange { .slot = static_cast<uint16_t>(slot), .glyph = _slots->glyphs[slot] });
    return changes;
}

std::shared_ptr<RegisteredGlyph const> GlyphGlossary::find(char32_t codepoint) const noexcept
{
    if (!_slots || !isGlyphRegistrationCodepoint(codepoint))
        return nullptr;
    return _slots->glyphs[codepoint - FirstGlyphRegistrationCodepoint];
}

std::shared_ptr<RegisteredGlyphs const> GlyphGlossary::snapshot() const
{
    if (empty())
        return nullptr;
    if (!_snapshot)
        _snapshot = std::make_shared<RegisteredGlyphs const>(_slots->glyphs);
    return _snapshot;
}

void GlyphGlossary::put(char32_t codepoint, std::shared_ptr<RegisteredGlyph const> glyph)
{
    // Clearing a slot of a glossary nothing was ever registered in needs no storage either.
    if (!isGlyphRegistrationCodepoint(codepoint) || (!_slots && !glyph))
        return;
    if (!_slots)
        _slots = std::make_unique<Slots>();
    auto const index = codepoint - FirstGlyphRegistrationCodepoint;
    auto& slot = _slots->glyphs[index];
    if (!slot && !glyph)
        return;
    _count += (glyph ? 1U : 0U);
    _count -= (slot ? 1U : 0U);
    slot = std::move(glyph);
    _slots->changedAt[index] = ++_generation;
    _snapshot.reset();
}

std::string executeGlyphProtocol(GlyphGlossary& glossary,
                                 SystemGlyphCoverage const& coverage,
                                 std::string_view body)
{
    auto command = parseGlyphCommand(body);
    if (!command)
        return std::visit(Refuser {}, command.error());
    // Moved from: a registration hands its glyph to the glossary instead of copying it.
    return std::visit(Executor { .glossary = glossary, .coverage = coverage }, std::move(*command));
}

} // namespace vtbackend
