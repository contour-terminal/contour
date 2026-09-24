// SPDX-License-Identifier: Apache-2.0
#include <core/Escape.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <format>
#include <ranges>
#include <string>
#include <string_view>

using namespace std::string_view_literals;

namespace
{
std::string allBytes()
{
    auto result = std::string {};
    for (auto const i: std::views::iota(0u, 256u))
        result.push_back(static_cast<char>(i));
    return result;
}
} // namespace

TEST_CASE("escape: the printable ASCII range passes through unchanged", "[base][escape]")
{
    // 0x20 (space) to 0x7E (tilde) inclusive is what a terminal prints as itself. 0x7E was
    // excluded, so `~` came out as a numeric escape.
    CHECK(core::escape(' ') == " ");
    CHECK(core::escape('A') == "A");
    CHECK(core::escape('}') == "}");
    CHECK(core::escape('~') == "~");

    // The two ends of the range are escaped.
    CHECK(core::escape(0x1F) == "\\x1f");
    CHECK(core::escape(0x7F) == "\\x7f");
}

TEST_CASE("escape: the named escapes win over the numeric ones", "[base][escape]")
{
    CHECK(core::escape('\\') == "\\\\");
    CHECK(core::escape('\t') == "\\t");
    CHECK(core::escape('\r') == "\\r");
    CHECK(core::escape('\n') == "\\n");
    CHECK(core::escape(0x1B) == "\\e");
    CHECK(core::escape('"') == "\\\"");
}

TEST_CASE("unescape: it reads back every form escape() writes", "[base][escape]")
{
    SECTION("named escapes")
    {
        CHECK(core::unescape("\\\\"sv) == "\\");
        CHECK(core::unescape("\\t"sv) == "\t");
        CHECK(core::unescape("\\r"sv) == "\r");
        CHECK(core::unescape("\\n"sv) == "\n");
        CHECK(core::unescape("\\e"sv) == "\x1B");
        // A quote is escaped on the way out, so it must be unescaped on the way back in.
        CHECK(core::unescape("\\\""sv) == "\"");
    }

    SECTION("hexadecimal escapes")
    {
        CHECK(core::unescape("\\x00"sv) == std::string(1, '\0'));
        CHECK(core::unescape("\\x41"sv) == "A");
        CHECK(core::unescape("\\x7f"sv) == "\x7F");
        CHECK(core::unescape("\\xff"sv) == "\xFF");
    }

    SECTION("octal escapes, whose leading digit is not always zero")
    {
        CHECK(core::unescape("\\000"sv) == std::string(1, '\0'));
        CHECK(core::unescape("\\001"sv) == "\x01");
        CHECK(core::unescape("\\101"sv) == "A");
        CHECK(core::unescape("\\177"sv) == "\x7F");
        CHECK(core::unescape("\\377"sv) == "\xFF");
    }

    SECTION("a numeric escape consumes exactly its digits")
    {
        CHECK(core::unescape("\\0015"sv)
              == "\x01"
                 "5");
        CHECK(core::unescape("\\x015"sv)
              == "\x01"
                 "5");
        CHECK(core::unescape("a\\x41b"sv) == "aAb");
    }

    SECTION("text without escapes is returned as it is")
    {
        CHECK(core::unescape(""sv).empty());
        CHECK(core::unescape("hello, world"sv) == "hello, world");
    }
}

// escape() and unescape() are each other's inverse, or the escaped form of a log line cannot be
// read back as the bytes that produced it.
TEST_CASE("escape and unescape round-trip every byte", "[base][escape]")
{
    SECTION("hexadecimal, byte by byte")
    {
        for (auto const i: std::views::iota(0u, 256u))
        {
            auto const ch = static_cast<uint8_t>(i);
            auto const escaped = core::escape(ch, core::NumericEscape::Hex);
            INFO(std::format("byte 0x{:02x} escaped as \"{}\"", i, escaped));
            auto const restored = core::unescape(escaped);
            REQUIRE(restored.size() == 1);
            CHECK(static_cast<uint8_t>(restored[0]) == ch);
        }
    }

    SECTION("octal, byte by byte")
    {
        for (auto const i: std::views::iota(0u, 256u))
        {
            auto const ch = static_cast<uint8_t>(i);
            auto const escaped = core::escape(ch, core::NumericEscape::Octal);
            INFO(std::format("byte 0x{:02x} escaped as \"{}\"", i, escaped));
            auto const restored = core::unescape(escaped);
            REQUIRE(restored.size() == 1);
            CHECK(static_cast<uint8_t>(restored[0]) == ch);
        }
    }

    SECTION("a whole string of every byte, in either numeric style")
    {
        auto const input = allBytes();
        CHECK(core::unescape(core::escape(input, core::NumericEscape::Hex)) == input);
        CHECK(core::unescape(core::escape(input, core::NumericEscape::Octal)) == input);
    }
}

TEST_CASE("escapeMarkdown: only the backtick is rewritten", "[base][escape]")
{
    CHECK(core::escapeMarkdown("plain"sv) == "plain");
    CHECK(core::escapeMarkdown("a`b"sv) == "a``` b");
}
