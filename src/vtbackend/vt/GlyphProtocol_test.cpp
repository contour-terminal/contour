// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/testing/GlyfRecordBuilder.hpp>
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
#include <vector>

using namespace vtbackend;
using namespace vtbackend::glyf_test;

namespace
{

/// @return A registration message for U+100000 with @p parameters between the codepoint and the payload.
[[nodiscard]] std::string registration(std::string_view parameters = {})
{
    return std::format(
        "25a1;r;cp=100000{}{};{}", parameters.empty() ? "" : ";", parameters, base64(squareRecord()));
}

/// @return The layout of a registration carrying @p parameters, when it is taken.
[[nodiscard]] std::optional<GlyphLayout> layoutOf(std::string_view parameters)
{
    auto const command = parseGlyphCommand(registration(parameters));
    if (!command || !std::holds_alternative<GlyphRegistration>(*command))
        return std::nullopt;
    return std::get<GlyphRegistration>(*command).layout;
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
          == std::unexpected { GlyphCommandError { GlyphClearRefused {} } });
}

TEST_CASE("GlyphProtocol: a registration carries its outline and the default layout", "[GlyphProtocol]")
{
    auto const command = parseGlyphCommand(registration());
    REQUIRE(command.has_value());
    auto const& taken = std::get<GlyphRegistration>(*command);
    CHECK(taken.codepoint == 0x100000);
    CHECK(taken.reply == GlyphReplyMode::All);
    CHECK(taken.layout == GlyphLayout {});
    CHECK(taken.outline.points.size() == 4);
    CHECK_FALSE(taken.layout.declaresExtent());
}

TEST_CASE("GlyphProtocol: sizing and placement parameters", "[GlyphProtocol]")
{
    auto const layout = layoutOf(
        "upm=2048;aw=1200;lh=2400;ymin=-400;width=2;size=advance;align=end,baseline;pad=0.1,0.2,0.3,0.4");
    REQUIRE(layout.has_value());
    CHECK(layout->unitsPerEm == 2048);
    CHECK(layout->advanceWidth == 1200);
    CHECK(layout->lineHeight == 2400);
    CHECK(layout->extentBottom == -400);
    CHECK(layout->span == GlyphRenderSpan::TwoCells);
    CHECK(layout->size == GlyphSizeMode::Advance);
    CHECK(layout->horizontal == GlyphHorizontalAlignment::End);
    CHECK(layout->vertical == GlyphVerticalAlignment::Baseline);
    CHECK(layout->padding == GlyphPadding { .top = 0.1F, .right = 0.2F, .bottom = 0.3F, .left = 0.4F });
    CHECK(layout->declaresExtent());

    // Any one of the three declares the extent.
    CHECK(layoutOf("ymin=0")->declaresExtent());

    // A pad that leaves no span is read as none, not refused.
    CHECK(layoutOf("pad=0,0.5,0,0.5")->padding == GlyphPadding {});

    // An unknown parameter is ignored.
    CHECK(layoutOf("future=1") == GlyphLayout {});
}

TEST_CASE("GlyphProtocol: an invalid sizing value refuses the registration", "[GlyphProtocol]")
{
    for (auto const parameters: {
             "upm=0",
             "upm=65536",
             "upm=abc",
             "aw=0",
             "lh=-1",
             "ymin=40000",
             "ymin=",
             "width=3",
             "size=fill",
             "align=center",
             "align=center,middle",
             "align=baseline,center",
             "pad=0,0,0",
             "pad=0,0,0,1.5",
             "pad=0,0,0,-0.1",
             "pad=a,0,0,0",
         })
    {
        INFO(parameters);
        auto const refused = refusalOf(registration(parameters));
        REQUIRE(refused.has_value());
        CHECK(refused->reason == GlyphRegisterError::MalformedPayload);
        CHECK(refused->codepoint == 0x100000);
    }
}

TEST_CASE("GlyphProtocol: why a registration is refused", "[GlyphProtocol]")
{
    auto const payload = base64(squareRecord());

    SECTION("a codepoint outside the block, before the payload is looked at")
    {
        auto const refused = refusalOf("25a1;r;cp=e0a0;not base64 at all");
        REQUIRE(refused.has_value());
        CHECK(refused->reason == GlyphRegisterError::OutOfNamespace);
        CHECK(refused->codepoint == 0xE0A0);
    }

    SECTION("a format this terminal does not advertise")
    {
        auto const refused = refusalOf(std::format("25a1;r;cp=100000;fmt=colrv1;{}", payload));
        REQUIRE(refused.has_value());
        CHECK(refused->reason == GlyphRegisterError::MalformedPayload);
    }

    SECTION("no payload, or one that is not base64")
    {
        CHECK(refusalOf("25a1;r;cp=100000;")->reason == GlyphRegisterError::MalformedPayload);
        CHECK(refusalOf("25a1;r;cp=100000")->reason == GlyphRegisterError::MalformedPayload);
        CHECK(refusalOf("25a1;r;cp=100000;!!!!")->reason == GlyphRegisterError::MalformedPayload);
    }

    SECTION("a payload over 64 KiB")
    {
        auto const large = std::vector<uint8_t>(MaxGlyphPayloadBytes + 3, uint8_t { 0 });
        auto const refused = refusalOf(std::format("25a1;r;cp=100000;{}", base64(large)));
        REQUIRE(refused.has_value());
        CHECK(refused->reason == GlyphRegisterError::PayloadTooLarge);
    }

    SECTION("a payload one byte over 64 KiB, which only decoding it can tell")
    {
        // 65,537 bytes and 65,536 share their count of base64 quartets, so the size judged from the
        // digits lets this one through to the decoded size.
        auto const large = std::vector<uint8_t>(MaxGlyphPayloadBytes + 1, uint8_t { 0 });
        auto const refused = refusalOf(std::format("25a1;r;cp=100000;{}", base64(large)));
        REQUIRE(refused.has_value());
        CHECK(refused->reason == GlyphRegisterError::PayloadTooLarge);

        auto const largest = std::vector<uint8_t>(MaxGlyphPayloadBytes, uint8_t { 0 });
        CHECK(parseGlyphCommand(std::format("25a1;r;cp=100000;{}", base64(largest))).has_value());
    }

    SECTION("a message sent without its payload still names its codepoint")
    {
        // The last part is a parameter, not a payload, wherever the codepoint stands among them.
        for (auto const body:
             { "25a1;r;reply=1;cp=100000", "25a1;r;cp=100000;upm=1000", "25a1;r;cp=100000;=" })
        {
            INFO(body);
            auto const refused = refusalOf(body);
            REQUIRE(refused.has_value());
            CHECK(refused->codepoint == 0x100000);
            CHECK(refused->reason == GlyphRegisterError::MalformedPayload);
        }
        // Padded base64 is still a payload.
        CHECK(parseGlyphCommand(std::format("25a1;r;cp=100000;{}", base64(std::vector<uint8_t>(10, 0))))
                  .has_value());
    }

    SECTION("a record the decoder refuses")
    {
        auto composite = squareRecord();
        composite[0] = 0xFF;
        auto const refused = refusalOf(std::format("25a1;r;cp=100000;{}", base64(composite)));
        REQUIRE(refused.has_value());
        CHECK(refused->reason == GlyphRegisterError::CompositeUnsupported);
    }

    SECTION("the reply mode is carried by the refusal")
    {
        CHECK(refusalOf("25a1;r;cp=e0a0;reply=0;AAAA")->reply == GlyphReplyMode::None);
        CHECK(refusalOf("25a1;r;cp=e0a0;reply=2;AAAA")->reply == GlyphReplyMode::ErrorsOnly);
        CHECK(refusalOf("25a1;r;cp=e0a0;reply=7;AAAA")->reply == GlyphReplyMode::All);
    }

    SECTION("without a codepoint there is nothing to answer")
    {
        auto const ignored = std::unexpected { GlyphCommandError { GlyphMessageIgnored {} } };
        CHECK(parseGlyphCommand(std::format("25a1;r;{}", payload)) == ignored);
        CHECK(parseGlyphCommand(std::format("25a1;r;cp=zz;{}", payload)) == ignored);
    }
}

TEST_CASE("GlyphProtocol: replies", "[GlyphProtocol]")
{
    using Glossary = GlyphSources::Glossary;
    using System = GlyphSources::System;

    CHECK(glyph_reply::support() == "\033_25a1;s;fmt=glyf\033\\");

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
    CHECK(glyph_reply::clearRefused() == "\033_25a1;c;status=1;reason=out_of_namespace\033\\");
}

TEST_CASE("GlyphProtocol: every refusal has a reason on the wire", "[GlyphProtocol]")
{
    CHECK(toString(GlyphRegisterError::OutOfNamespace) == "out_of_namespace");
    CHECK(toString(GlyphRegisterError::CompositeUnsupported) == "composite_unsupported");
    CHECK(toString(GlyphRegisterError::HintingUnsupported) == "hinting_unsupported");
    CHECK(toString(GlyphRegisterError::MalformedPayload) == "malformed_payload");
    CHECK(toString(GlyphRegisterError::PayloadTooLarge) == "payload_too_large");
    CHECK(toString(GlyphRegisterError::OutlineTooLarge) == "outline_too_large");
}
