// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/InputGenerator.h>

#include <crispy/escape.h>

#include <catch2/catch.hpp>

#include <string>

#include <libunicode/convert.h>

using namespace std;
using vtbackend::InputGenerator;
using vtbackend::Modifier;
using Buffer = vtbackend::InputGenerator::Sequence;
using crispy::escape;

TEST_CASE("InputGenerator.Modifier.encodings")
{
    // Ensures we can construct the correct values that are needed
    // as parameters for input events in the VT protocol.

    auto constexpr Alt = Modifier(Modifier::Alt);
    auto constexpr Shift = Modifier(Modifier::Shift);
    auto constexpr Control = Modifier(Modifier::Control);
    auto constexpr Super = Modifier(Modifier::Super);

    CHECK((1 + Shift.value()) == 2);
    CHECK((1 + Alt.value()) == 3);
    CHECK((1 + (Shift | Alt).value()) == 4);
    CHECK((1 + Control.value()) == 5);
    CHECK((1 + (Shift | Control).value()) == 6);
    CHECK((1 + (Alt | Control).value()) == 7);
    CHECK((1 + (Shift | Alt | Control).value()) == 8);
    CHECK((1 + Super.value()) == 9);
    CHECK((1 + (Super | Shift).value()) == 10);
    CHECK((1 + (Super | Alt).value()) == 11);
    CHECK((1 + (Super | Alt | Shift).value()) == 12);
    CHECK((1 + (Super | Control).value()) == 13);
    CHECK((1 + (Super | Control | Shift).value()) == 14);
    CHECK((1 + (Super | Control | Alt).value()) == 15);
    CHECK((1 + (Super | Control | Alt | Shift).value()) == 16);
}

TEST_CASE("InputGenerator.consume")
{
    auto received = string {};
    auto input = InputGenerator {};
    input.generateRaw("ABCDEF"sv);
    REQUIRE(input.peek() == "ABCDEF"sv);
    input.consume(2);
    REQUIRE(input.peek() == "CDEF"sv);
    input.consume(3);
    REQUIRE(input.peek() == "F"sv);

    input.generateRaw("abcdef"sv);
    REQUIRE(input.peek() == "Fabcdef"sv);
    input.consume(7);
    REQUIRE(input.peek() == ""sv);
}

TEST_CASE("InputGenerator.Ctrl+Space", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.generate(L' ', Modifier::Control);
    CHECK(escape(input.peek()) == "\\x00");
}

TEST_CASE("InputGenerator.Ctrl+A", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.generate('A', Modifier::Control);
    auto const c0 = string(1, static_cast<char>(0x01));
    REQUIRE(escape(input.peek()) == escape(c0));
}

TEST_CASE("InputGenerator.Ctrl+D", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.generate('D', Modifier::Control);
    auto const c0 = string(1, static_cast<char>(0x04));
    REQUIRE(escape(input.peek()) == escape(c0));
}

TEST_CASE("InputGenerator.Ctrl+[", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.generate('[', Modifier::Control);
    auto const c0 = string(1, static_cast<char>(0x1b)); // 27
    REQUIRE(escape(input.peek()) == escape(c0));
}

TEST_CASE("InputGenerator.Ctrl+\\", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.generate('\\', Modifier::Control);
    auto const c0 = string(1, static_cast<char>(0x1c)); // 28
    REQUIRE(escape(input.peek()) == escape(c0));
}

TEST_CASE("InputGenerator.Ctrl+]", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.generate(']', Modifier::Control);
    auto const c0 = string(1, static_cast<char>(0x1d)); // 29
    REQUIRE(escape(input.peek()) == escape(c0));
}

TEST_CASE("InputGenerator.Ctrl+^", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.generate('^', Modifier::Control);
    auto const c0 = string(1, static_cast<char>(0x1e)); // 30
    REQUIRE(escape(input.peek()) == escape(c0));
}

TEST_CASE("InputGenerator.Ctrl+_", "[terminal,input]")
{
    auto input = InputGenerator {};
    input.generate('_', Modifier::Control);
    auto const c0 = string(1, static_cast<char>(0x1f)); // 31
    REQUIRE(escape(input.peek()) == escape(c0));
}

TEST_CASE("InputGenerator.all(Ctrl + A..Z)", "[terminal,input]")
{
    for (char ch = 'A'; ch <= 'Z'; ++ch)
    {
        INFO(fmt::format("Testing Ctrl+{}", ch));
        auto input = InputGenerator {};
        input.generate(static_cast<char32_t>(ch), Modifier::Control);
        auto const c0 = string(1, static_cast<char>(ch - 'A' + 1));
        REQUIRE(escape(input.peek()) == escape(c0));
    }
}
