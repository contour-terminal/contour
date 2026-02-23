// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Functions.h>
#include <vtbackend/InputGenerator.h>
#include <vtbackend/MockTerm.h>
#include <vtbackend/Screen.h>
#include <vtbackend/primitives.h>
#include <vtbackend/test_helpers.h>

#include <crispy/base64.h>
#include <crispy/escape.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>

using crispy::escape;
using namespace vtbackend;
using namespace vtbackend::test;
using namespace std;
using namespace std::string_view_literals;

// NOLINTBEGIN(misc-const-correctness,readability-function-cognitive-complexity)

// {{{ Mode enable/disable and feature detection

TEST_CASE("BinaryPaste.mode_enable_disable", "[binary_paste]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };

    // Initially disabled
    REQUIRE_FALSE(mock.terminal.isModeEnabled(DECMode::BinaryPaste));
    REQUIRE_FALSE(mock.terminal.isBinaryPasteModeEnabled());

    // Enable via DECSM
    mock.writeToScreen(DECSM(2033));
    REQUIRE(mock.terminal.isModeEnabled(DECMode::BinaryPaste));
    REQUIRE(mock.terminal.isBinaryPasteModeEnabled());

    // Disable via DECRM
    mock.writeToScreen(DECRM(2033));
    REQUIRE_FALSE(mock.terminal.isModeEnabled(DECMode::BinaryPaste));
    REQUIRE_FALSE(mock.terminal.isBinaryPasteModeEnabled());
}

TEST_CASE("BinaryPaste.DECRQM_query", "[binary_paste]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };

    // Query when disabled — expect mode value 2 (reset)
    mock.writeToScreen(DECRQM(2033));
    CHECK(e(mock.terminal.peekInput()) == e("\033[?2033;2$y"sv));
    mock.terminal.flushInput();

    // Enable the mode
    mock.writeToScreen(DECSM(2033));
    mock.terminal.flushInput();

    // Query when enabled — expect mode value 1 (set)
    mock.writeToScreen(DECRQM(2033));
    CHECK(e(mock.terminal.peekInput()) == e("\033[?2033;1$y"sv));
}

// }}} Mode enable/disable and feature detection

// {{{ Data delivery (DCS 2033 ; <size> b d<mime>;<base64> ST)

TEST_CASE("BinaryPaste.generateBinaryPaste_produces_DCS", "[binary_paste]")
{
    auto input = InputGenerator {};
    auto constexpr TestBytes = std::array<uint8_t, 16> { 't', 'e', 's', 't', ' ', 'b', 'i', 'n',
                                                         'a', 'r', 'y', ' ', 'd', 'a', 't', 'a' };
    auto const testData = std::span<uint8_t const>(TestBytes);
    input.generateBinaryPaste("image/png", testData);

    auto const output = std::string(input.peek());
    // Verify DCS structure: ESC P 2033 ; <size> b d <mime> ; <base64> ESC backslash
    auto const prefix = std::format("\033P2033;{}bdimage/png;", testData.size());
    CHECK(output.starts_with(prefix));
    CHECK(output.ends_with("\033\\"));

    // Extract and verify base64 payload decodes back to original
    auto const mimeEnd = output.find(';', prefix.size() - 1);
    auto const base64Start = mimeEnd + 1;
    auto const base64End = output.size() - 2; // before ESC backslash
    auto const base64Data = output.substr(base64Start, base64End - base64Start);
    auto const decoded = crispy::base64::decode(base64Data);
    CHECK(decoded == std::string_view(reinterpret_cast<char const*>(testData.data()), testData.size()));
}

TEST_CASE("BinaryPaste.generateBinaryPaste_empty_data", "[binary_paste]")
{
    auto input = InputGenerator {};
    input.generateBinaryPaste("image/png", std::span<uint8_t const> {});
    CHECK(input.peek().empty());
}

TEST_CASE("BinaryPaste.sendBinaryPaste_via_terminal", "[binary_paste]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };

    // Enable binary paste mode
    mock.writeToScreen(DECSM(2033));
    mock.resetReplyData();

    // Send binary paste (this calls flushInput(), writing to PTY stdin buffer)
    auto constexpr TestBytes = std::array<uint8_t, 8> { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    auto const testData = std::span<uint8_t const>(TestBytes);
    mock.terminal.sendBinaryPaste("image/png", testData);

    auto const& output = mock.replyData();
    auto const prefix = std::format("\033P2033;{}bdimage/png;", testData.size());
    CHECK(output.starts_with(prefix));
    CHECK(output.ends_with("\033\\"));
}

// }}} Data delivery

// {{{ Reset behavior

TEST_CASE("BinaryPaste.hard_reset_clears_mode", "[binary_paste]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };

    // Enable the mode
    mock.writeToScreen(DECSM(2033));
    REQUIRE(mock.terminal.isModeEnabled(DECMode::BinaryPaste));

    // Hard reset (RIS)
    mock.writeToScreen("\033c");
    CHECK_FALSE(mock.terminal.isModeEnabled(DECMode::BinaryPaste));
}

TEST_CASE("BinaryPaste.soft_reset_clears_mode_and_preferences", "[binary_paste]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };

    // Enable the mode and configure MIME preferences
    mock.writeToScreen(DECSM(2033));
    mock.writeToScreen("\033P2033bcimage/png,image/jpeg\033\\");
    REQUIRE(mock.terminal.isModeEnabled(DECMode::BinaryPaste));
    REQUIRE(mock.terminal.binaryPasteMimePreferences().size() == 2);

    // Soft reset (DECSTR)
    mock.writeToScreen("\033[!p");
    CHECK_FALSE(mock.terminal.isModeEnabled(DECMode::BinaryPaste));
    CHECK(mock.terminal.binaryPasteMimePreferences().empty());
}

TEST_CASE("BinaryPaste.reset_clears_input_generator_flag", "[binary_paste]")
{
    auto input = InputGenerator {};

    // Enable binary paste
    input.setBinaryPaste(true);
    REQUIRE(input.binaryPaste());

    // Reset
    input.reset();
    CHECK_FALSE(input.binaryPaste());
}

// }}} Reset behavior

// {{{ MIME Preference Configuration (DCS 2033 b c<mime-list> ST)

TEST_CASE("BinaryPaste.configure_mime_preferences", "[binary_paste]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };

    // Enable binary paste mode
    mock.writeToScreen(DECSM(2033));

    // Send MIME preference configuration: DCS 2033 b c<mime-list> ST
    mock.writeToScreen("\033P2033bcimage/png,image/jpeg,text/html\033\\");

    auto const prefs = mock.terminal.binaryPasteMimePreferences();
    REQUIRE(prefs.size() == 3);
    CHECK(prefs[0] == "image/png");
    CHECK(prefs[1] == "image/jpeg");
    CHECK(prefs[2] == "text/html");
}

TEST_CASE("BinaryPaste.configure_reset_to_defaults", "[binary_paste]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };

    // Enable and configure
    mock.writeToScreen(DECSM(2033));
    mock.writeToScreen("\033P2033bcimage/png,text/html\033\\");
    REQUIRE(mock.terminal.binaryPasteMimePreferences().size() == 2);

    // Send empty configure to reset to defaults
    mock.writeToScreen("\033P2033bc\033\\");
    CHECK(mock.terminal.binaryPasteMimePreferences().empty());
}

TEST_CASE("BinaryPaste.configure_clears_on_mode_disable", "[binary_paste]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };

    // Enable and configure
    mock.writeToScreen(DECSM(2033));
    mock.writeToScreen("\033P2033bcimage/png,image/jpeg\033\\");
    REQUIRE(mock.terminal.binaryPasteMimePreferences().size() == 2);

    // Disable mode — preferences should be cleared
    mock.writeToScreen(DECRM(2033));
    CHECK(mock.terminal.binaryPasteMimePreferences().empty());
}

TEST_CASE("BinaryPaste.configure_clears_on_hard_reset", "[binary_paste]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };

    // Enable and configure
    mock.writeToScreen(DECSM(2033));
    mock.writeToScreen("\033P2033bcimage/png\033\\");
    REQUIRE(mock.terminal.binaryPasteMimePreferences().size() == 1);

    // Hard reset (RIS)
    mock.writeToScreen("\033c");
    CHECK(mock.terminal.binaryPasteMimePreferences().empty());
}

TEST_CASE("BinaryPaste.configure_ignored_when_mode_disabled", "[binary_paste]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };

    // Mode is not enabled — send configure sequence anyway
    mock.writeToScreen("\033P2033bcimage/png\033\\");

    // Preferences should remain empty (configure was silently ignored)
    CHECK(mock.terminal.binaryPasteMimePreferences().empty());
}

TEST_CASE("BinaryPaste.configure_arbitrary_mime_types", "[binary_paste]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };

    // Enable mode
    mock.writeToScreen(DECSM(2033));

    // Configure with non-image MIME types
    mock.writeToScreen("\033P2033bctext/html,application/json,text/plain\033\\");

    auto const prefs = mock.terminal.binaryPasteMimePreferences();
    REQUIRE(prefs.size() == 3);
    CHECK(prefs[0] == "text/html");
    CHECK(prefs[1] == "application/json");
    CHECK(prefs[2] == "text/plain");
}

TEST_CASE("BinaryPaste.configure_updates_replace_previous", "[binary_paste]")
{
    auto mock = MockTerm { PageSize { LineCount(3), ColumnCount(10) } };

    // Enable and configure
    mock.writeToScreen(DECSM(2033));
    mock.writeToScreen("\033P2033bcimage/png,image/jpeg\033\\");
    REQUIRE(mock.terminal.binaryPasteMimePreferences().size() == 2);

    // Send another configure — replaces previous preferences
    mock.writeToScreen("\033P2033bctext/html\033\\");
    auto const prefs = mock.terminal.binaryPasteMimePreferences();
    REQUIRE(prefs.size() == 1);
    CHECK(prefs[0] == "text/html");
}

// }}} MIME Preference Configuration

// NOLINTEND(misc-const-correctness,readability-function-cognitive-complexity)
