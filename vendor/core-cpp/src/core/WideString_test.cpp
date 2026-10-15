// SPDX-License-Identifier: Apache-2.0
//
// core::toUtf8() is the one UTF-16 to UTF-8 conversion behind every wide Win32 read in core-cpp: an
// environment value, the environment's names, a thread's description, the host name, a path spelled
// in a message. core::toUtf16() is the other direction, behind every wide write and every path built
// from a string. Each case below tells one apart from a conversion that is wrong in one particular
// way. core::systemErrorMessage() reads the system's message for an error through the first.
#include <core/Escape.hpp>
#include <core/WideString.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>

#include <windows.h>

using namespace std::string_view_literals;

using core::systemErrorMessage;
using core::toUtf16;
using core::toUtf8;

namespace
{

/// The two halves of U+1F600. A lone surrogate has no `\u` spelling, so the cases build text from
/// code units.
constexpr auto HighSurrogate = wchar_t { 0xD83D };
constexpr auto LowSurrogate = wchar_t { 0xDE00 };

/// U+FFFD, the replacement character, in UTF-8.
constexpr auto Replacement = std::string_view { "\xEF\xBF\xBD" };

/// Has the system's messages for this thread read in the languages @p names lists, for as long as
/// it lives: each name ends in a NUL, and the list in another. A test's thread prefers no language
/// of its own, so it is given none back.
class ScopedThreadLanguages
{
  public:
    explicit ScopedThreadLanguages(wchar_t const* names)
    {
        auto count = ULONG { 0 };
        std::ignore = SetThreadPreferredUILanguages(MUI_LANGUAGE_NAME, names, &count);
    }
    ~ScopedThreadLanguages() { std::ignore = SetThreadPreferredUILanguages(0, nullptr, nullptr); }
    ScopedThreadLanguages(ScopedThreadLanguages const&) = delete;
    ScopedThreadLanguages(ScopedThreadLanguages&&) = delete;
    ScopedThreadLanguages& operator=(ScopedThreadLanguages const&) = delete;
    ScopedThreadLanguages& operator=(ScopedThreadLanguages&&) = delete;
};

/// @return Whether the system has its messages in @p language, asked without the function under
///         test: a machine has those of the languages installed on it, and usually US English.
[[nodiscard]] bool hasMessagesIn(LANGID language)
{
    auto buffer = std::array<wchar_t, 256> {};
    return FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                          nullptr,
                          ERROR_INVALID_HANDLE,
                          language,
                          buffer.data(),
                          static_cast<DWORD>(buffer.size()),
                          nullptr)
           != 0;
}

} // namespace

TEST_CASE("toUtf8 converts empty text to empty text", "[WideString][windows]")
{
    CHECK(toUtf8(L"").empty());
    CHECK(toUtf8(std::wstring_view {}).empty());
}

TEST_CASE("toUtf8 keeps ASCII as it is", "[WideString][windows]")
{
    CHECK(toUtf8(L"build-agent-07.example") == "build-agent-07.example");
}

TEST_CASE("toUtf8 encodes the BMP at two and three bytes", "[WideString][windows]")
{
    CHECK(toUtf8(L"Jürgen") == "J\xC3\xBCrgen");
    CHECK(toUtf8(L"Ω") == "\xCE\xA9");
    CHECK(toUtf8(L"€") == "\xE2\x82\xAC");
}

TEST_CASE("toUtf8 encodes a surrogate pair as one four-byte sequence", "[WideString][windows]")
{
    // Not CESU-8's two three-byte halves, which a conversion one code unit at a time produces.
    CHECK(toUtf8(std::wstring { HighSurrogate, LowSurrogate }) == "\xF0\x9F\x98\x80");
}

TEST_CASE("toUtf8 reads a lone surrogate as U+FFFD and converts the rest", "[WideString][windows]")
{
    auto const replaced = "a" + std::string { Replacement } + "z";
    CHECK(toUtf8(std::wstring { L'a', HighSurrogate, L'z' }) == replaced);
    CHECK(toUtf8(std::wstring { L'a', LowSurrogate, L'z' }) == replaced);
    // A pair in the wrong order is two lone surrogates.
    CHECK(toUtf8(std::wstring { LowSurrogate, HighSurrogate })
          == std::string { Replacement } + std::string { Replacement });
}

TEST_CASE("toUtf8 converts the whole view and an embedded NUL with it", "[WideString][windows]")
{
    CHECK(toUtf8(std::wstring_view { L"a\0b", 3 }) == std::string_view { "a\0b", 3 });
}

TEST_CASE("toUtf16 converts UTF-8 of every sequence length", "[WideString][windows]")
{
    CHECK(toUtf16("") == std::wstring {});
    CHECK(toUtf16("build-agent-07.example") == L"build-agent-07.example");
    CHECK(toUtf16("J\xC3\xBCrgen") == L"Jürgen");
    CHECK(toUtf16("\xE2\x82\xAC") == L"€");
    CHECK(toUtf16("\xF0\x9F\x98\x80") == std::wstring { HighSurrogate, LowSurrogate });
    CHECK(toUtf16(std::string_view { "a\0b", 3 }) == std::wstring_view { L"a\0b", 3 });
}

TEST_CASE("toUtf16 refuses text that is not UTF-8", "[WideString][windows]")
{
    // Strict, unlike toUtf8(): read as U+FFFD, such text would name another file or variable. The
    // error is the one pathFromUtf8() answers, so a caller passes it on as it is.
    for (auto const text: {
             "J\xFCrgen"sv,          // "Jürgen" in CP-1252
             "a\x80z"sv,             // a lone continuation byte
             "a\xE2\x82"sv,          // a sequence cut short
             "a\xED\xA0\x80z"sv,     // an encoded surrogate
             "a\xC0\xAFz"sv,         // an overlong form
             "a\xF4\x90\x80\x80z"sv, // past U+10FFFF
         })
    {
        INFO("text: " << core::escape(text));
        auto const utf16 = toUtf16(text);
        REQUIRE_FALSE(utf16.has_value());
        CHECK(utf16.error() == std::errc::illegal_byte_sequence);
    }
}

TEST_CASE("systemErrorMessage spells the system's message without its line break", "[WideString][windows]")
{
    if (!hasMessagesIn(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US)))
        SKIP("the system has no US English messages to compare with");
    auto const english = ScopedThreadLanguages { L"en-US\0" };
    // FormatMessageW() answers "The system cannot find the file specified.", then CR LF.
    CHECK(systemErrorMessage(ERROR_FILE_NOT_FOUND) == "The system cannot find the file specified.");
    CHECK(systemErrorMessage(ERROR_ACCESS_DENIED) == "Access is denied.");
}

TEST_CASE("systemErrorMessage spells a message beyond ASCII in UTF-8", "[WideString][windows]")
{
    // The German message holds a U+00FC, which FormatMessageA() spells in the ANSI code page: on a
    // CP-1252 host as the byte 0xFC, which UTF-8 has no reading for. The thread asks for German, as
    // a German user's does, and gets it where the system has it.
    if (!hasMessagesIn(MAKELANGID(LANG_GERMAN, SUBLANG_GERMAN)))
        SKIP("the system has no German messages, the language this case reads one beyond ASCII in");
    auto const german = ScopedThreadLanguages { L"de-DE\0" };
    CHECK(systemErrorMessage(ERROR_INVALID_HANDLE) == "Das Handle ist ung\xC3\xBCltig.");
}

TEST_CASE("systemErrorMessage names an error the system has no message for", "[WideString][windows]")
{
    // Bit 29 marks an error an application defined, which the system's table has no message for.
    CHECK(systemErrorMessage(0x2000'0001UL) == "unknown error 536870913");
}
