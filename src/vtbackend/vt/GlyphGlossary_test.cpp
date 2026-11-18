// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/testing/GlyfRecordBuilder.hpp>
#include <vtbackend/vt/GlyphGlossary.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

using namespace vtbackend;
using namespace vtbackend::glyf_test;

namespace
{

/// A font that has the Nerd Fonts branch icon and nothing else.
class BranchIconOnly final: public SystemGlyphCoverage
{
  public:
    [[nodiscard]] GlyphSources::System systemCoverage(char32_t codepoint) const override
    {
        return codepoint == 0xE0A0 ? GlyphSources::System::Present : GlyphSources::System::Absent;
    }
};

[[nodiscard]] GlyphOutline square()
{
    return decodeGlyf(squareRecord()).value();
}

/// Sends @p body to @p glossary, with no font behind it.
[[nodiscard]] std::string send(GlyphGlossary& glossary, std::string_view body)
{
    return executeGlyphProtocol(glossary, NoSystemGlyphCoverage {}, body);
}

[[nodiscard]] std::string registerSquareAt(std::string_view codepoint, std::string_view parameters = {})
{
    return std::format(
        "25a1;r;cp={}{}{};{}", codepoint, parameters.empty() ? "" : ";", parameters, base64(squareRecord()));
}

} // namespace

TEST_CASE("GlyphGlossary: register, find, clear", "[GlyphGlossary]")
{
    auto glossary = GlyphGlossary {};
    CHECK(glossary.empty());
    CHECK(glossary.find(0x100000) == nullptr);

    glossary.registerGlyph(0x100000, GlyphLayout {}, square());
    glossary.registerGlyph(0x1003FF, GlyphLayout {}, square());
    CHECK_FALSE(glossary.empty());
    REQUIRE(glossary.find(0x100000) != nullptr);
    CHECK(glossary.find(0x100000)->outline.points.size() == 4);
    CHECK(glossary.find(0x100001) == nullptr);
    CHECK(glossary.find(U'a') == nullptr);

    glossary.clear(0x100000);
    CHECK(glossary.find(0x100000) == nullptr);
    CHECK_FALSE(glossary.empty());

    // Clearing an empty slot, or a codepoint outside the block, changes nothing.
    glossary.clear(0x100000);
    glossary.clear(U'a');
    CHECK(glossary.find(0x1003FF) != nullptr);

    glossary.clear();
    CHECK(glossary.empty());
}

TEST_CASE("GlyphGlossary: clearing what is clear is not a change", "[GlyphGlossary]")
{
    // An application clearing on its way out must not cost a redraw when it registered nothing.
    auto glossary = GlyphGlossary {};
    auto const generation = glossary.generation();
    glossary.clear();
    glossary.clear(0x100000);
    CHECK(glossary.generation() == generation);

    glossary.registerGlyph(0x100000, GlyphLayout {}, square());
    glossary.clear(0x100001); // another slot, empty
    CHECK(glossary.generation() == generation + 1);
    glossary.clear(0x100000);
    CHECK(glossary.generation() == generation + 2);
}

TEST_CASE("GlyphGlossary: a glyph's id follows its content", "[GlyphGlossary]")
{
    auto glossary = GlyphGlossary {};
    glossary.registerGlyph(0x100000, GlyphLayout {}, square());
    glossary.registerGlyph(0x100001, GlyphLayout {}, square());
    glossary.registerGlyph(0x100002, GlyphLayout { .size = GlyphSizeMode::Stretch }, square());
    glossary.registerGlyph(0x100003, GlyphLayout { .extentBottom = 0 }, square());

    auto other = square();
    other.points[2].x = 900;
    glossary.registerGlyph(0x100004, GlyphLayout {}, other);

    // The same content shares its tiles, wherever it is registered ...
    CHECK(glossary.find(0x100000)->id == glossary.find(0x100001)->id);
    // ... and anything that changes what is drawn does not: a stale tile is never served.
    CHECK(glossary.find(0x100000)->id != glossary.find(0x100002)->id);
    CHECK(glossary.find(0x100000)->id != glossary.find(0x100003)->id);
    CHECK(glossary.find(0x100000)->id != glossary.find(0x100004)->id);
}

TEST_CASE("GlyphGlossary: a snapshot does not change under a frame", "[GlyphGlossary]")
{
    auto glossary = GlyphGlossary {};
    CHECK(glossary.snapshot() == nullptr); // nothing registered, nothing for a frame to carry

    glossary.registerGlyph(0x100000, GlyphLayout {}, square());
    auto const frame = glossary.snapshot();
    REQUIRE(frame != nullptr);
    auto const* const drawn = registeredGlyphOf(frame.get(), U"\U00100000");
    REQUIRE(drawn != nullptr);

    // Unchanged, the same snapshot serves the next frame.
    CHECK(glossary.snapshot() == frame);

    auto const generation = glossary.generation();
    glossary.clear(0x100000);
    CHECK(glossary.generation() > generation);
    CHECK(glossary.snapshot() == nullptr);
    // The frame on display still holds the glyph it was built with.
    CHECK(registeredGlyphOf(frame.get(), U"\U00100000") == drawn);
}

TEST_CASE("GlyphGlossary: changes since an earlier state", "[GlyphGlossary]")
{
    // What the daemon sends a client that already holds the glossary as it stood.
    auto const slotsOf = [](std::vector<GlyphChange> const& changes) {
        return std::ranges::to<std::vector>(changes | std::views::transform(&GlyphChange::slot));
    };
    auto glossary = GlyphGlossary {};
    CHECK(glossary.changesSince(0).empty());

    glossary.registerGlyph(0x100002, GlyphLayout {}, square());
    glossary.registerGlyph(0x100000, GlyphLayout {}, square());
    auto const first = glossary.generation();
    // Everything, ascending, for one that holds nothing.
    CHECK(slotsOf(glossary.changesSince(0)) == std::vector<uint16_t> { 0, 2 });
    CHECK(glossary.changesSince(first).empty());

    // A replaced slot and a cleared one, and not the slot left alone.
    glossary.registerGlyph(0x100005, GlyphLayout {}, square());
    glossary.clear(0x100002);
    auto const changes = glossary.changesSince(first);
    REQUIRE(slotsOf(changes) == std::vector<uint16_t> { 2, 5 });
    CHECK(changes[0].glyph == nullptr);
    CHECK(changes[1].glyph == glossary.find(0x100005));

    // Clearing everything is a change of every slot that held a glyph, and of no other.
    auto const second = glossary.generation();
    glossary.clear();
    auto const cleared = glossary.changesSince(second);
    CHECK(slotsOf(cleared) == std::vector<uint16_t> { 0, 5 });
    CHECK(std::ranges::all_of(cleared, [](auto const& change) { return change.glyph == nullptr; }));
}

TEST_CASE("registeredGlyphOf: only a cell that is one codepoint of the block", "[GlyphGlossary]")
{
    auto glossary = GlyphGlossary {};
    glossary.registerGlyph(0x100000, GlyphLayout {}, square());
    auto const frame = glossary.snapshot();

    CHECK(registeredGlyphOf(frame.get(), U"\U00100000") != nullptr);
    CHECK(registeredGlyphOf(frame.get(), U"\U00100001") == nullptr); // nothing registered there
    CHECK(registeredGlyphOf(frame.get(), U"\U00100000́") == nullptr); // it never joins a cluster
    CHECK(registeredGlyphOf(frame.get(), U"a") == nullptr);
    CHECK(registeredGlyphOf(frame.get(), U"") == nullptr);
    CHECK(registeredGlyphOf(nullptr, U"\U00100000") == nullptr);
}

TEST_CASE("executeGlyphProtocol: support and query", "[GlyphGlossary]")
{
    auto glossary = GlyphGlossary {};
    CHECK(send(glossary, "25a1;s") == "\033_25a1;s;fmt=glyf\033\\");

    CHECK(send(glossary, "25a1;q;cp=100000") == "\033_25a1;q;cp=100000;status=\033\\");
    CHECK(send(glossary, registerSquareAt("100000")) == "\033_25a1;r;cp=100000;status=0\033\\");
    CHECK(send(glossary, "25a1;q;cp=100000") == "\033_25a1;q;cp=100000;status=glossary\033\\");

    // What the fonts draw comes from whoever knows them.
    auto const font = BranchIconOnly {};
    CHECK(executeGlyphProtocol(glossary, font, "25a1;q;cp=e0a0")
          == "\033_25a1;q;cp=e0a0;status=system\033\\");
    CHECK(executeGlyphProtocol(glossary, font, "25a1;q;cp=e0a1") == "\033_25a1;q;cp=e0a1;status=\033\\");

    // The grapheme query is not advertised, and says so when asked anyway.
    CHECK(send(glossary, "25a1;q;grapheme=1f468,200d,1f469")
          == "\033_25a1;q;grapheme=1f468,200d,1f469;reason=unsupported\033\\");
    CHECK(send(glossary, "25a1;q") == "\033_25a1;q;reason=malformed_query\033\\");
}

TEST_CASE("executeGlyphProtocol: the reply mode of a registration", "[GlyphGlossary]")
{
    auto glossary = GlyphGlossary {};

    // reply=0: nothing, taken or refused.
    CHECK(send(glossary, registerSquareAt("100000", "reply=0")).empty());
    CHECK(glossary.find(0x100000) != nullptr);
    CHECK(send(glossary, registerSquareAt("e0a0", "reply=0")).empty());

    // reply=2: refusals only.
    CHECK(send(glossary, registerSquareAt("100001", "reply=2")).empty());
    CHECK(glossary.find(0x100001) != nullptr);
    CHECK(send(glossary, registerSquareAt("e0a0", "reply=2"))
          == "\033_25a1;r;cp=e0a0;status=1;reason=out_of_namespace\033\\");

    // A refused registration leaves the glossary as it was.
    CHECK(glossary.find(0xE0A0) == nullptr);
}

TEST_CASE("executeGlyphProtocol: a second registration overwrites the first", "[GlyphGlossary]")
{
    auto glossary = GlyphGlossary {};
    REQUIRE_FALSE(send(glossary, registerSquareAt("100000")).empty());
    auto const first = glossary.find(0x100000);
    REQUIRE_FALSE(send(glossary, registerSquareAt("100000", "size=stretch")).empty());
    auto const second = glossary.find(0x100000);
    REQUIRE(second != nullptr);
    CHECK(second->layout.size == GlyphSizeMode::Stretch);
    CHECK(second->id != first->id);
}

TEST_CASE("executeGlyphProtocol: clear", "[GlyphGlossary]")
{
    auto glossary = GlyphGlossary {};
    REQUIRE_FALSE(send(glossary, registerSquareAt("100000")).empty());
    REQUIRE_FALSE(send(glossary, registerSquareAt("100001")).empty());

    CHECK(send(glossary, "25a1;c;cp=100000") == "\033_25a1;c;cp=100000;status=0\033\\");
    CHECK(glossary.find(0x100000) == nullptr);
    CHECK(glossary.find(0x100001) != nullptr);

    // Clearing an empty slot succeeds.
    CHECK(send(glossary, "25a1;c;cp=100000") == "\033_25a1;c;cp=100000;status=0\033\\");

    CHECK(send(glossary, "25a1;c;cp=e0a0") == "\033_25a1;c;status=1;reason=out_of_namespace\033\\");
    CHECK(glossary.find(0x100001) != nullptr);

    CHECK(send(glossary, "25a1;c") == "\033_25a1;c;status=0\033\\");
    CHECK(glossary.empty());
}

TEST_CASE("executeGlyphProtocol: every slot of the block can be held at once", "[GlyphGlossary]")
{
    auto glossary = GlyphGlossary {};
    for (auto const slot: std::views::iota(size_t { 0 }, GlyphRegistrationSlots))
        glossary.registerGlyph(
            static_cast<char32_t>(FirstGlyphRegistrationCodepoint + slot), GlyphLayout {}, square());
    auto const frame = glossary.snapshot();
    REQUIRE(frame != nullptr);
    CHECK(std::ranges::none_of(*frame, [](auto const& glyph) { return glyph == nullptr; }));
}
