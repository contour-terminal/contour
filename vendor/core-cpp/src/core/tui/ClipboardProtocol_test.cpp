// SPDX-License-Identifier: Apache-2.0
#include <core/tui/ClipboardProtocol.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <string_view>

using core::tui::ClipboardTarget;
using core::tui::ClipboardWriteError;
using core::tui::describe;
using core::tui::encodeOsc52;
using core::tui::encodeOsc5522Write;
using core::tui::isPlainTextMimeType;
using core::tui::parseOsc5522WriteStatus;

namespace
{

/// @brief Counts how often @p needle occurs in @p haystack, without overlap.
auto countOf(std::string_view haystack, std::string_view needle) -> std::size_t
{
    auto count = std::size_t { 0 };
    auto pos = haystack.find(needle);
    while (pos != std::string_view::npos)
    {
        ++count;
        pos = haystack.find(needle, pos + needle.size());
    }
    return count;
}

/// The OSC 5522 data packets' common prefix for text/plain ("text/plain" is dGV4dC9wbGFpbg==).
constexpr auto PlainTextDataPacket = std::string_view { "\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==;" };

} // namespace

TEST_CASE("tui.ClipboardProtocol: OSC 52 carries the base64 of the data and names the selection")
{
    CHECK(encodeOsc52("hi", ClipboardTarget::Clipboard) == "\033]52;c;aGk=\033\\");
    CHECK(encodeOsc52("hi", ClipboardTarget::Primary) == "\033]52;p;aGk=\033\\");
    CHECK(encodeOsc52("", ClipboardTarget::Clipboard) == "\033]52;c;\033\\");
    CHECK(encodeOsc52("\xC3\xA4", ClipboardTarget::Clipboard) == "\033]52;c;w6Q=\033\\");
}

TEST_CASE("tui.ClipboardProtocol: an OSC 5522 write is a header, the data packets and an end packet")
{
    CHECK(encodeOsc5522Write("hi", "text/plain", ClipboardTarget::Clipboard)
          == "\033]5522;type=write\033\\"
             "\033]5522;type=wdata:mime=dGV4dC9wbGFpbg==;aGk=\033\\"
             "\033]5522;type=wdata\033\\");
}

TEST_CASE("tui.ClipboardProtocol: an OSC 5522 write to the primary selection says loc=primary")
{
    auto const sequence = encodeOsc5522Write("hi", "text/plain", ClipboardTarget::Primary);
    CHECK(sequence.starts_with("\033]5522;type=write:loc=primary\033\\"));
}

TEST_CASE("tui.ClipboardProtocol: OSC 5522 data is split into chunks of at most 4096 raw bytes")
{
    // Empty data still declares its MIME type, in one empty packet: the end packet alone carries none.
    auto const empty = encodeOsc5522Write("", "text/plain", ClipboardTarget::Clipboard);
    CHECK(countOf(empty, PlainTextDataPacket) == 1);
    CHECK(empty.contains(std::string(PlainTextDataPacket) + "\033\\"));

    auto const exact = encodeOsc5522Write(std::string(4096, 'a'), "text/plain", ClipboardTarget::Clipboard);
    CHECK(countOf(exact, PlainTextDataPacket) == 1);

    // One byte past the chunk size is a second packet carrying that byte alone ("a" is YQ==).
    auto const over = encodeOsc5522Write(std::string(4097, 'a'), "text/plain", ClipboardTarget::Clipboard);
    CHECK(countOf(over, PlainTextDataPacket) == 2);
    CHECK(over.contains(std::string(PlainTextDataPacket) + "YQ==\033\\"));
    CHECK(over.ends_with("\033]5522;type=wdata\033\\"));
}

TEST_CASE("tui.ClipboardProtocol: a write status reply is DONE or the error it names")
{
    struct Row
    {
        std::string_view reply;
        std::optional<ClipboardWriteError> error; // nullopt: success
    };
    auto const rows = std::array {
        Row { .reply = "5522;type=write:status=DONE", .error = std::nullopt },
        Row { .reply = "5522;status=DONE:type=write", .error = std::nullopt },
        Row { .reply = "5522;type=write:status=EPERM", .error = ClipboardWriteError::PermissionDenied },
        Row { .reply = "5522;type=write:status=EFBIG", .error = ClipboardWriteError::TooLarge },
        Row { .reply = "5522;type=write:status=EBUSY", .error = ClipboardWriteError::Busy },
        Row { .reply = "5522;type=write:status=EINVAL", .error = ClipboardWriteError::InvalidData },
        Row { .reply = "5522;type=write:status=ENOSYS", .error = ClipboardWriteError::PrimaryUnavailable },
        Row { .reply = "5522;type=write:status=EIO", .error = ClipboardWriteError::IoError },
        Row { .reply = "5522;type=write:status=EWHATEVER", .error = ClipboardWriteError::IoError },
    };
    for (auto const& row: rows)
    {
        CAPTURE(row.reply);
        auto const status = parseOsc5522WriteStatus(row.reply);
        REQUIRE(status.has_value());
        if (row.error)
            CHECK(status->error() == *row.error);
        else
            CHECK(status->has_value());
    }
}

TEST_CASE("tui.ClipboardProtocol: a reply that is not an OSC 5522 write status is not one")
{
    CHECK_FALSE(parseOsc5522WriteStatus("52;c;aGk=").has_value());
    CHECK_FALSE(parseOsc5522WriteStatus("5522;type=read:status=OK").has_value());
    CHECK_FALSE(parseOsc5522WriteStatus("5522;type=write").has_value());
    CHECK_FALSE(parseOsc5522WriteStatus("55220;type=write:status=DONE").has_value());
    CHECK_FALSE(parseOsc5522WriteStatus("").has_value());
}

TEST_CASE("tui.ClipboardProtocol: every error has its own description")
{
    auto const errors = std::array {
        ClipboardWriteError::NoTerminal,
        ClipboardWriteError::UnsupportedMimeType,
        ClipboardWriteError::PermissionDenied,
        ClipboardWriteError::TooLarge,
        ClipboardWriteError::Busy,
        ClipboardWriteError::InvalidData,
        ClipboardWriteError::IoError,
        ClipboardWriteError::PrimaryUnavailable,
        ClipboardWriteError::NoConfirmation,
    };
    auto seen = std::set<std::string_view> {};
    for (auto const error: errors)
    {
        auto const text = describe(error);
        CHECK_FALSE(text.empty());
        CHECK(seen.insert(text).second);
    }
}

TEST_CASE("tui.ClipboardProtocol: only text/plain, with or without parameters, is plain text")
{
    CHECK(isPlainTextMimeType("text/plain"));
    CHECK(isPlainTextMimeType("text/plain;charset=utf-8"));
    CHECK_FALSE(isPlainTextMimeType("text/html"));
    CHECK_FALSE(isPlainTextMimeType("image/png"));
    CHECK_FALSE(isPlainTextMimeType("text/plainx"));
}
