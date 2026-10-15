// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/vt/MacroDefinition.hpp>

#include <catch2/catch_test_macros.hpp>

#include <expected>
#include <optional>
#include <string>
#include <string_view>

using namespace vtbackend;

namespace
{

using CollectedBody = std::expected<std::string, MacroDefinitionError>;

/// Hands @p payload to a collector with @p room bytes of room, as the parser would, and ends the string.
[[nodiscard]] CollectedBody collect(MacroEncoding encoding, size_t room, std::string_view payload)
{
    auto result = CollectedBody {};
    auto collector =
        MacroBodyCollector { encoding, room, [&](CollectedBody body) { result = std::move(body); } };
    collector.pass(payload);
    collector.finalize();
    return result;
}

} // namespace

TEST_CASE("decodeMacroDefinitionParameters", "[macro]")
{
    // DEC STD 070: "Pid must be a number between 0 and 63; Pdt 0 or 1; and Pen 0 or 1; otherwise the control
    // will be ignored."
    using Error = MacroDefinitionError;
    using Parameters = MacroDefinitionParameters;

    CHECK(decodeMacroDefinitionParameters(0, 0, 0) == Parameters {});
    CHECK(decodeMacroDefinitionParameters(63, 1, 1)
          == Parameters { .id = 63, .deletion = MacroDeletion::All, .encoding = MacroEncoding::HexPairs });
    CHECK(decodeMacroDefinitionParameters(64, 0, 0) == std::unexpected { Error::InvalidId });
    CHECK(decodeMacroDefinitionParameters(0, 2, 0) == std::unexpected { Error::InvalidDeletion });
    CHECK(decodeMacroDefinitionParameters(0, 0, 2) == std::unexpected { Error::InvalidEncoding });
}

TEST_CASE("MacroBodyCollector.decodesTheBodyAsItIsSpelled", "[macro]")
{
    CHECK(collect(MacroEncoding::Text, 16, "Hi") == CollectedBody { "Hi" });
    CHECK(collect(MacroEncoding::HexPairs, 16, "4869") == CollectedBody { "Hi" });
    CHECK(collect(MacroEncoding::HexPairs, 16, "4869a") == CollectedBody { "Hi" }); // half a pair is dropped
}

TEST_CASE("MacroBodyCollector.refusesAHexBodyThatIsNotHex", "[macro]")
{
    // DEC STD 070: "If a non-hex pair is encountered that is not a valid repeat sequence, the entire macro
    // definition is ignored." A text body knows no such rule.
    auto const notHex = CollectedBody { std::unexpected { MacroDefinitionError::NotHex } };

    CHECK(collect(MacroEncoding::HexPairs, 16, "48G9") == notHex);
    CHECK(collect(MacroEncoding::HexPairs, 16, "484G") == notHex);
    CHECK(collect(MacroEncoding::Text, 16, "G9") == CollectedBody { "G9" });
}

TEST_CASE("MacroBodyCollector.repeatsWhatARepeatSequenceEncloses", "[macro]")
{
    // "! Pn ; D...D ;" (DEC STD 070), in a hex body only.
    auto const tooLarge = CollectedBody { std::unexpected { MacroDefinitionError::TooLarge } };

    CHECK(collect(MacroEncoding::HexPairs, 64, "41!;42;43") == CollectedBody { "ABC" }); // Pn omitted: once
    CHECK(collect(MacroEncoding::HexPairs, 64, "!0;41;") == CollectedBody { "A" });      // as omitted
    CHECK(collect(MacroEncoding::HexPairs, 64, "41!3;4243;44") == CollectedBody { "ABCBCBCD" });
    CHECK(collect(MacroEncoding::HexPairs, 64, "41!2;42") == CollectedBody { "ABB" }); // ends with the string
    CHECK(collect(MacroEncoding::HexPairs, 5, "!5;41;") == CollectedBody { "AAAAA" });
    CHECK(collect(MacroEncoding::HexPairs, 4, "!5;41;") == tooLarge);
    CHECK(collect(MacroEncoding::HexPairs, 4, "!99999999999999999999;41;") == tooLarge);
    CHECK(collect(MacroEncoding::Text, 64, "!3;A;") == CollectedBody { "!3;A;" });
}

TEST_CASE("MacroBodyCollector.refusesAMalformedRepeatSequence", "[macro]")
{
    // DEC STD 070: "If any non parameter characters are encountered before the first semicolon, the entire
    // macro definition is ignored", and "If a non-hex character is encountered in the repeat string, the
    // entire macro definition is ignored."
    auto const notHex = CollectedBody { std::unexpected { MacroDefinitionError::NotHex } };

    CHECK(collect(MacroEncoding::HexPairs, 64, "!x;41;") == notHex);
    CHECK(collect(MacroEncoding::HexPairs, 64, "!2;4G;") == notHex);
    CHECK(collect(MacroEncoding::HexPairs, 64, "!2;41!3;42;;") == notHex); // a repeat within a repeat
    CHECK(collect(MacroEncoding::HexPairs, 64, "41;") == notHex);          // a semicolon outside one
}

TEST_CASE("MacroBodyCollector.leavesControlCharactersOut", "[macro]")
{
    // They format the control string; they are not part of the macro (DEC STD 070).
    CHECK(collect(MacroEncoding::Text, 16, "H\r\n\ti") == CollectedBody { "Hi" });
    CHECK(collect(MacroEncoding::HexPairs, 16, "4\r\n8\t69\x01") == CollectedBody { "Hi" });
}

TEST_CASE("MacroBodyCollector.holdsNoMoreThanItsRoom", "[macro]")
{
    auto const tooLarge = CollectedBody { std::unexpected { MacroDefinitionError::TooLarge } };

    SECTION("a body that just fits")
    {
        CHECK(collect(MacroEncoding::Text, 4, "abcd") == CollectedBody { "abcd" });
        CHECK(collect(MacroEncoding::HexPairs, 2, "4142") == CollectedBody { "AB" });
    }
    SECTION("a body a byte too large is refused, not cut")
    {
        CHECK(collect(MacroEncoding::Text, 4, "abcde") == tooLarge);
        CHECK(collect(MacroEncoding::HexPairs, 2, "414243") == tooLarge);
    }
    SECTION("what arrives past the room is consumed, not stored")
    {
        auto result = CollectedBody {};
        auto collector = MacroBodyCollector { MacroEncoding::Text, 4, [&](CollectedBody body) {
                                                 result = std::move(body);
                                             } };

        collector.pass(std::string(size_t { 1024 } * 1024, 'x'));

        CHECK(collector.heldBytes() == 0);
        collector.finalize();
        CHECK(result == tooLarge);
    }
}
