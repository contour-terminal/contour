// SPDX-License-Identifier: Apache-2.0
#include <core/platform/FileUri.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <string_view>

using namespace core::platform;
using namespace std::string_view_literals;

// ---------------------------------------------------------------------------
// percentEncodeUriPath
// ---------------------------------------------------------------------------

TEST_CASE("FileUri.percentEncode.unreserved_and_separator_pass_through")
{
    CHECK(percentEncodeUriPath("/home/me/file-1.2_v~3") == "/home/me/file-1.2_v~3");
    CHECK(percentEncodeUriPath("").empty());
}

TEST_CASE("FileUri.percentEncode.space_and_reserved_bytes")
{
    struct Row
    {
        std::string_view input;
        std::string_view expected;
    };

    // Reserved and delimiter bytes must not survive raw: `#`/`?` would truncate the URI, and
    // ESC/`;` would break out of the OSC payload the URI is embedded in.
    constexpr auto Rows = std::array {
        Row { .input = "/a b"sv, .expected = "/a%20b"sv },
        Row { .input = "/a#b"sv, .expected = "/a%23b"sv },
        Row { .input = "/a?b"sv, .expected = "/a%3Fb"sv },
        Row { .input = "/a%b"sv, .expected = "/a%25b"sv },
        Row { .input = "/a;b"sv, .expected = "/a%3Bb"sv },
        Row { .input = "/a\nb"sv, .expected = "/a%0Ab"sv },
        Row { .input = "/a\033b"sv, .expected = "/a%1Bb"sv },
        Row { .input = "/a:b"sv, .expected = "/a%3Ab"sv },
    };

    for (auto const& row: Rows)
        CHECK(percentEncodeUriPath(row.input) == row.expected);
}

TEST_CASE("FileUri.percentEncode.utf8_is_encoded_bytewise")
{
    // "ä" is C3 A4 in UTF-8; RFC 3986 encodes the individual bytes.
    CHECK(percentEncodeUriPath("/tmp/\xC3\xA4") == "/tmp/%C3%A4");
    // A high byte that std::isalnum could accept under a non-C locale must still be encoded.
    CHECK(percentEncodeUriPath("/tmp/\xE4") == "/tmp/%E4");
}

TEST_CASE("FileUri.percentEncode.hex_digits_are_uppercase")
{
    CHECK(percentEncodeUriPath("/\xFF") == "/%FF");
}

// ---------------------------------------------------------------------------
// fileUri
// ---------------------------------------------------------------------------

TEST_CASE("FileUri.fileUri.posix_path_with_host")
{
    CHECK(fileUri("/home/me/projects", "box") == "file://box/home/me/projects");
}

TEST_CASE("FileUri.fileUri.empty_host_yields_local_form")
{
    // RFC 8089: an empty authority is equivalent to "localhost".
    CHECK(fileUri("/tmp/x", "") == "file:///tmp/x");
}

TEST_CASE("FileUri.fileUri.empty_path_yields_empty_uri")
{
    // Callers use the empty result as "do not emit a hyperlink at all".
    CHECK(fileUri("", "box").empty());
    CHECK(fileUri("", "").empty());
}

TEST_CASE("FileUri.fileUri.encodes_path")
{
    CHECK(fileUri("/home/me/My Docs", "box") == "file://box/home/me/My%20Docs");
    CHECK(fileUri("/tmp/\xC3\xA4", "box") == "file://box/tmp/%C3%A4");
}

TEST_CASE("FileUri.fileUri.windows_drive_gains_leading_slash")
{
    // The scheme requires the path component to begin with '/', which a drive-letter path
    // does not. The colon stays literal: it is a valid pchar and is what terminals expect.
    CHECK(fileUri("C:/Users/me", "box") == "file://box/C:/Users/me");
    CHECK(fileUri("c:/tmp", "") == "file:///c:/tmp");
}

TEST_CASE("FileUri.fileUri.unc_path_takes_authority_from_path")
{
    // The file lives on `srv`, so the local hostname is irrelevant.
    CHECK(fileUri("//srv/share/x", "box") == "file://srv/share/x");
    CHECK(fileUri("//srv/share/x", "") == "file://srv/share/x");
}

TEST_CASE("FileUri.fileUri.fragment_is_appended_and_encoded")
{
    CHECK(fileUri("/a/b.cpp", "box", "42") == "file://box/a/b.cpp#42");
    CHECK(fileUri("/a/b.cpp", "box", "") == "file://box/a/b.cpp");
    CHECK(fileUri("/a", "box", "x y") == "file://box/a#x%20y");
}
