// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/vt/GlyphProtocol.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <variant>

using namespace vtbackend;

namespace
{

/// A square, as path data in a 24-unit viewBox.
constexpr auto Square = std::string_view { "fill=nonzero,fg:M4 4h16v16H4Z" };

/// @return A registration message for U+100000 with @p parameters between the codepoint and the payload.
[[nodiscard]] std::string registration(std::string_view parameters = "fmt=vg;vb=0,0,24,24",
                                       std::string_view payload = Square)
{
    return std::format("25a1;r;cp=100000{}{};{}", parameters.empty() ? "" : ";", parameters, payload);
}

/// @return The layout of a registration carrying @p parameters beside its format and viewBox, when taken.
[[nodiscard]] std::optional<GlyphLayout> layoutOf(std::string_view parameters)
{
    auto const command = parseGlyphCommand(registration(std::format("fmt=vg;vb=0,0,24,24;{}", parameters)));
    if (!command || !std::holds_alternative<GlyphRegistration>(*command))
        return std::nullopt;
    return std::get<GlyphRegistration>(*command).definition.layout;
}

/// @return Why @p body is refused as a registration, when it is.
[[nodiscard]] std::optional<GlyphRegistrationRefused> refusalOf(std::string_view body)
{
    auto const command = parseGlyphCommand(body);
    if (command || !std::holds_alternative<GlyphRegistrationRefused>(command.error()))
        return std::nullopt;
    return std::get<GlyphRegistrationRefused>(command.error());
}

} // namespace

TEST_CASE("GlyphProtocol: a message is told by its identifier", "[GlyphProtocol]")
{
    CHECK(isGlyphProtocol("25a1;s"));
    CHECK_FALSE(isGlyphProtocol("25a1"));
    CHECK_FALSE(isGlyphProtocol("25a1s"));
    CHECK_FALSE(isGlyphProtocol("Ga=q"));
    CHECK_FALSE(isGlyphProtocol(""));
}

TEST_CASE("GlyphProtocol: the registration block is U+100000 to U+1003FF", "[GlyphProtocol]")
{
    CHECK_FALSE(isGlyphRegistrationCodepoint(0xFFFFF));
    CHECK(isGlyphRegistrationCodepoint(0x100000));
    CHECK(isGlyphRegistrationCodepoint(0x1003FF));
    CHECK_FALSE(isGlyphRegistrationCodepoint(0x100400));
    CHECK_FALSE(isGlyphRegistrationCodepoint(0xE0A0));
}

TEST_CASE("GlyphProtocol: verbs", "[GlyphProtocol]")
{
    CHECK(parseGlyphCommand("25a1;s") == GlyphCommand { GlyphSupportRequest {} });
    CHECK(parseGlyphCommand("25a1;s;future=1") == GlyphCommand { GlyphSupportRequest {} });
    CHECK(parseGlyphCommand("25a1;c") == GlyphCommand { GlyphClearRequest {} });
    CHECK(parseGlyphCommand("25a1;c;reply=2")
          == GlyphCommand {
              GlyphClearRequest { .codepoint = std::nullopt, .reply = GlyphReplyMode::ErrorsOnly } });
    CHECK(parseGlyphCommand("25a1;c;cp=100000;reply=0")
          == GlyphCommand { GlyphClearRequest { .codepoint = 0x100000, .reply = GlyphReplyMode::None } });
    CHECK(parseGlyphCommand("25a1;c;cp=1003ff")
          == GlyphCommand { GlyphClearRequest { .codepoint = 0x1003FF } });
    CHECK(parseGlyphCommand("25a1;q;cp=E0A0")
          == GlyphCommand { GlyphCodepointQuery { .codepoint = 0xE0A0 } });
    CHECK(parseGlyphCommand("25a1;q;grapheme=1f468,200d,1f469")
          == GlyphCommand { GlyphGraphemeQuery { .codepoints = U"\U0001F468‍\U0001F469" } });

    CHECK(parseGlyphCommand("25a1;x") == std::unexpected { GlyphCommandError { GlyphMessageIgnored {} } });
    CHECK(parseGlyphCommand("25a1;") == std::unexpected { GlyphCommandError { GlyphMessageIgnored {} } });
}

TEST_CASE("GlyphProtocol: a query names one codepoint or one cluster", "[GlyphProtocol]")
{
    auto const malformed = std::unexpected { GlyphCommandError { GlyphQueryMalformed {} } };
    CHECK(parseGlyphCommand("25a1;q") == malformed);
    CHECK(parseGlyphCommand("25a1;q;cp=41;grapheme=41") == malformed);
    CHECK(parseGlyphCommand("25a1;q;cp=xyz") == malformed);
    CHECK(parseGlyphCommand("25a1;q;cp=d800") == malformed);   // a surrogate
    CHECK(parseGlyphCommand("25a1;q;cp=110000") == malformed); // past the last codepoint
    CHECK(parseGlyphCommand("25a1;q;cp=41,42") == malformed);
    CHECK(parseGlyphCommand("25a1;q;grapheme=41,,42") == malformed);
    CHECK(parseGlyphCommand("25a1;q;grapheme=") == malformed); // a cluster holds at least one codepoint

    auto cluster = std::string { "25a1;q;grapheme=41" };
    for ([[maybe_unused]] auto const _: std::views::iota(0, 31))
        cluster += ",301";
    CHECK(parseGlyphCommand(cluster).has_value()); // 32 codepoints
    CHECK(parseGlyphCommand(cluster + ",301") == malformed);
}

TEST_CASE("GlyphProtocol: a clear outside the block is refused", "[GlyphProtocol]")
{
    CHECK(parseGlyphCommand("25a1;c;cp=e0a0")
          == std::unexpected { GlyphCommandError { GlyphClearRefused { .codepoint = 0xE0A0 } } });
    // The refusal carries the reply mode, as a registration's does.
    CHECK(parseGlyphCommand("25a1;c;cp=e0a0;reply=0")
          == std::unexpected { GlyphCommandError {
              GlyphClearRefused { .codepoint = 0xE0A0, .reply = GlyphReplyMode::None } } });
}

TEST_CASE("GlyphProtocol: a registration carries its glyph, its payload and the default layout",
          "[GlyphProtocol]")
{
    auto const command = parseGlyphCommand(registration());
    REQUIRE(command.has_value());
    auto const& taken = std::get<GlyphRegistration>(*command);
    CHECK(taken.codepoint == 0x100000);
    CHECK(taken.reply == GlyphReplyMode::All);
    CHECK(taken.definition.layout == GlyphLayout {});
    CHECK(taken.definition.glyph.viewBox == VectorViewBox { .x = 0, .y = 0, .width = 24, .height = 24 });
    REQUIRE(taken.definition.glyph.layers.size() == 1);
    CHECK(taken.definition.glyph.layers.front().segments.size() == 5);
    // The payload is kept as it was sent, for whoever the glyph travels on to.
    CHECK(taken.definition.payload == Square);
}

TEST_CASE("GlyphProtocol: placement keys", "[GlyphProtocol]")
{
    auto const layout = layoutOf("width=2;fit=cover;align=end,start");
    REQUIRE(layout.has_value());
    CHECK(layout->span == GlyphRenderSpan::TwoCells);
    CHECK(layout->fit == GlyphFit::Cover);
    CHECK(layout->horizontal == GlyphAlignment::End);
    CHECK(layout->vertical == GlyphAlignment::Start);
    CHECK(layoutOf("fit=stretch")->fit == GlyphFit::Stretch);
    CHECK(layoutOf("fit=contain") == GlyphLayout {});

    // The font formats' keys mean nothing to `vg`, and are ignored like any key it does not know --
    // whatever their value.
    CHECK(layoutOf("upm=0;aw=0;lh=-1;ymin=x;size=fill;pad=9,9,9,9;future=1") == GlyphLayout {});
}

TEST_CASE("GlyphProtocol: an invalid placement value refuses the registration", "[GlyphProtocol]")
{
    for (auto const parameters: {
             "width=3",
             "width=0",
             "fit=height", // a font format's size mode, not a fit
             "fit=fill",
             "align=center",
             "align=center,middle",
             "align=center,baseline", // a viewBox has no baseline
             "align=start,center,end",
         })
    {
        INFO(parameters);
        auto const refused = refusalOf(registration(std::format("fmt=vg;vb=0,0,24,24;{}", parameters)));
        REQUIRE(refused.has_value());
        CHECK(refused->reason == GlyphRegisterError::MalformedPayload);
        CHECK(refused->codepoint == 0x100000);
    }
}

TEST_CASE("GlyphProtocol: why a registration is refused", "[GlyphProtocol]")
{
    SECTION("a codepoint outside the block, before anything else is looked at")
    {
        auto const refused = refusalOf("25a1;r;cp=e0a0;fmt=nope;not a payload");
        REQUIRE(refused.has_value());
        CHECK(refused->reason == GlyphRegisterError::OutOfNamespace);
        CHECK(refused->codepoint == 0xE0A0);
    }

    SECTION("a format this terminal does not list")
    {
        for (auto const format: { "fmt=glyf", "fmt=colrv0", "fmt=colrv1", "fmt=svg", "fmt=" })
        {
            INFO(format);
            auto const refused = refusalOf(registration(std::format("{};vb=0,0,24,24", format)));
            REQUIRE(refused.has_value());
            CHECK(refused->reason == GlyphRegisterError::UnsupportedFormat);
        }
        // A registration without `fmt` is `glyf`, as it was before formats were negotiated.
        CHECK(refusalOf(registration("vb=0,0,24,24"))->reason == GlyphRegisterError::UnsupportedFormat);
        CHECK(refusalOf("25a1;r;cp=100000;AAAA")->reason == GlyphRegisterError::UnsupportedFormat);
    }

    SECTION("no viewBox, or one that is not a viewBox")
    {
        CHECK(refusalOf(registration("fmt=vg"))->reason == GlyphRegisterError::MalformedPayload);
        CHECK(refusalOf(registration("fmt=vg;vb=0,0,24"))->reason == GlyphRegisterError::MalformedPayload);
        CHECK(refusalOf(registration("fmt=vg;vb=0,0,0,24"))->reason == GlyphRegisterError::MalformedPayload);
    }

    SECTION("a payload that is not `vg`")
    {
        CHECK(refusalOf(registration("fmt=vg;vb=0,0,24,24", ""))->reason
              == GlyphRegisterError::MalformedPayload);
        CHECK(refusalOf(registration("fmt=vg;vb=0,0,24,24", "AAAA"))->reason
              == GlyphRegisterError::MalformedPayload);
        CHECK(refusalOf(registration("fmt=vg;vb=0,0,24,24", "fill=nonzero,fg:L1 1"))->reason
              == GlyphRegisterError::MalformedPayload);
    }

    SECTION("a payload past one of the format's limits")
    {
        CHECK(refusalOf(registration("fmt=vg;vb=0,0,24,24", "fill=nonzero,fg:M0 0L100 100"))->reason
              == GlyphRegisterError::OutlineTooLarge);
    }

    SECTION("a payload over 32 KiB")
    {
        // Valid `vg`, and too long: it is refused for its size before it is read.
        auto large = std::string { "fill=nonzero,fg:M1 1" };
        large.append(MaxGlyphPayloadBytes, ' ');
        CHECK(refusalOf(registration("fmt=vg;vb=0,0,24,24", large + "h1"))->reason
              == GlyphRegisterError::PayloadTooLarge);

        // Exactly the cap is read; this one is then found to be malformed, a moveto that never comes.
        CHECK(refusalOf(registration("fmt=vg;vb=0,0,24,24", std::string(MaxGlyphPayloadBytes, 'x')))->reason
              == GlyphRegisterError::MalformedPayload);
    }

    SECTION("the payload is the last field, whatever it holds")
    {
        // By position, not by content: `vg` text holds `=` and `,` of its own.
        CHECK(
            parseGlyphCommand(registration("fmt=vg;vb=0,0,24,24", "fill=nonzero,fg@0.5:M1 1h1")).has_value());
        // So a key after the payload is not a key: it is the payload, and the payload before it a key
        // nothing knows.
        CHECK(refusalOf(std::format("25a1;r;cp=100000;fmt=vg;{};vb=0,0,24,24", Square))->reason
              == GlyphRegisterError::MalformedPayload);
        // And a codepoint in the last field is not one: there is nothing to answer.
        auto const ignored = std::unexpected { GlyphCommandError { GlyphMessageIgnored {} } };
        CHECK(parseGlyphCommand("25a1;r;fmt=vg;cp=100000") == ignored);
        CHECK(parseGlyphCommand("25a1;r;cp=100000") == ignored);
    }

    SECTION("the reply mode is carried by the refusal")
    {
        CHECK(refusalOf("25a1;r;cp=e0a0;reply=0;x")->reply == GlyphReplyMode::None);
        CHECK(refusalOf("25a1;r;cp=e0a0;reply=2;x")->reply == GlyphReplyMode::ErrorsOnly);
        CHECK(refusalOf("25a1;r;cp=e0a0;reply=7;x")->reply == GlyphReplyMode::All);
    }

    SECTION("without a codepoint there is nothing to answer")
    {
        auto const ignored = std::unexpected { GlyphCommandError { GlyphMessageIgnored {} } };
        CHECK(parseGlyphCommand(std::format("25a1;r;fmt=vg;vb=0,0,24,24;{}", Square)) == ignored);
        CHECK(parseGlyphCommand(std::format("25a1;r;cp=zz;fmt=vg;vb=0,0,24,24;{}", Square)) == ignored);
    }
}

TEST_CASE("GlyphProtocol: a message cut off for its length is answered from its front", "[GlyphProtocol]")
{
    // What arrives of an over-long registration: the verb, the parameters, and the start of a payload.
    CHECK(answerOversizedGlyphMessage("25a1;r;cp=100000;fmt=vg;vb=0,0,24,24;fill=nonzero,fg:M1 1")
          == "\033_25a1;r;cp=100000;status=1;reason=payload_too_large\033\\");
    CHECK(answerOversizedGlyphMessage("25a1;r;reply=2;cp=100001;fill=nonzero")
          == "\033_25a1;r;cp=100001;status=1;reason=payload_too_large\033\\");

    // Outside the block it is refused for that, as it would have been at any size.
    CHECK(answerOversizedGlyphMessage("25a1;r;cp=e0a0;fill=nonzero")
          == "\033_25a1;r;cp=e0a0;status=1;reason=out_of_namespace\033\\");

    // No reply was asked for, there is no codepoint to name, or it is not a registration at all.
    CHECK(answerOversizedGlyphMessage("25a1;r;cp=100000;reply=0;fill=nonzero").empty());
    CHECK(answerOversizedGlyphMessage("25a1;r;fill=nonzero").empty());
    CHECK(answerOversizedGlyphMessage("25a1;q;cp=100000;fill=nonzero").empty());
    CHECK(answerOversizedGlyphMessage("25a1;r").empty());
    CHECK(answerOversizedGlyphMessage("Ga=T,f=32;AAAAAAAA").empty());
}

TEST_CASE("GlyphProtocol: replies", "[GlyphProtocol]")
{
    using Glossary = GlyphSources::Glossary;
    using System = GlyphSources::System;

    CHECK(glyph_reply::support() == "\033_25a1;s;fmt=vg\033\\");

    CHECK(glyph_reply::query(0xE0A0, {}) == "\033_25a1;q;cp=e0a0;status=\033\\");
    CHECK(glyph_reply::query(0xE0A0, { .system = System::Present })
          == "\033_25a1;q;cp=e0a0;status=system\033\\");
    CHECK(glyph_reply::query(0x100000, { .glossary = Glossary::Present })
          == "\033_25a1;q;cp=100000;status=glossary\033\\");
    CHECK(glyph_reply::query(0x100000, { .system = System::Present, .glossary = Glossary::Present })
          == "\033_25a1;q;cp=100000;status=system,glossary\033\\");

    CHECK(glyph_reply::graphemeUnsupported(U"\U0001F468‍\U0001F469")
          == "\033_25a1;q;grapheme=1f468,200d,1f469;reason=unsupported\033\\");
    CHECK(glyph_reply::queryMalformed() == "\033_25a1;q;reason=malformed_query\033\\");

    CHECK(glyph_reply::registered(0x100001) == "\033_25a1;r;cp=100001;status=0\033\\");
    CHECK(glyph_reply::registrationRefused(0xE0A0, GlyphRegisterError::OutOfNamespace)
          == "\033_25a1;r;cp=e0a0;status=1;reason=out_of_namespace\033\\");

    CHECK(glyph_reply::cleared(std::nullopt) == "\033_25a1;c;status=0\033\\");
    CHECK(glyph_reply::cleared(0x100001) == "\033_25a1;c;cp=100001;status=0\033\\");
    CHECK(glyph_reply::clearRefused(0xE0A0) == "\033_25a1;c;cp=e0a0;status=1;reason=out_of_namespace\033\\");
}

TEST_CASE("GlyphProtocol: every refusal has a reason on the wire", "[GlyphProtocol]")
{
    CHECK(toString(GlyphRegisterError::OutOfNamespace) == "out_of_namespace");
    CHECK(toString(GlyphRegisterError::UnsupportedFormat) == "unsupported_format");
    CHECK(toString(GlyphRegisterError::MalformedPayload) == "malformed_payload");
    CHECK(toString(GlyphRegisterError::PayloadTooLarge) == "payload_too_large");
    CHECK(toString(GlyphRegisterError::OutlineTooLarge) == "outline_too_large");
}
