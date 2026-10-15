// SPDX-License-Identifier: Apache-2.0
#pragma once

// Catch2 prints UTF-32 text one `{?}` per codepoint, so a failing check on a built or rendered text shows
// nothing of the text. These print it as the UTF-8 it encodes, quoted like any other string.
//
// Force-included into every file of a test binary (vtbackend_force_include_test_printers() in
// src/vtbackend/CMakeLists.txt) rather than included by the tests that want it. A printer that some files
// of a binary see and others do not is two definitions of the same Catch2 instantiations,
// Catch::Detail::stringify<std::u32string> among them, and the linker keeps whichever it meets first, for
// every file: a test that included this header still printed `{?}`.

#include <libunicode/convert.h>

#include <catch2/catch_tostring.hpp>

#include <cstddef>
#include <string>
#include <string_view>

/// Prints a UTF-32 string as the text it holds.
template <>
struct Catch::StringMaker<std::u32string>
{
    static std::string convert(std::u32string const& text)
    {
        return StringMaker<std::string>::convert(unicode::convert_to<char>(std::u32string_view(text)));
    }
};

/// Prints a UTF-32 string literal as the text it holds, up to its terminating null.
template <std::size_t Size>
struct Catch::StringMaker<char32_t[Size]>
{
    static std::string convert(char32_t const* text)
    {
        auto const literal = std::u32string_view(text, Size);
        return StringMaker<std::string>::convert(
            unicode::convert_to<char>(literal.substr(0, literal.find(U'\0'))));
    }
};
