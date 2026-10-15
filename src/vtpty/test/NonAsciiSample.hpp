// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <string_view>

namespace vtpty::testing
{

/// A file or directory name beyond ASCII, for the tests that check a path is read and spelled as written.
///
/// Characters of two, three and four UTF-8 bytes (ü, €, 李, 😀), most of them in no ANSI code page: a
/// narrow std::filesystem::path, and every "A" API, reads a string in that code page on Windows, where
/// such a name comes out as one that does not exist, or throws.
inline constexpr auto NonAsciiName = std::u8string_view { u8"ü€李\U0001F600" };

/// NonAsciiName's UTF-8 bytes, for the APIs that take them as a narrow string.
inline constexpr auto NonAsciiNameUtf8 =
    std::string_view { "\xc3\xbc\xe2\x82\xac\xe6\x9d\x8e\xf0\x9f\x98\x80" };

static_assert(std::ranges::equal(NonAsciiName, NonAsciiNameUtf8, [](char8_t utf8, char byte) {
    return utf8 == static_cast<char8_t>(byte);
}));

} // namespace vtpty::testing
