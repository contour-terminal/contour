// SPDX-License-Identifier: Apache-2.0
#include <core/tui/detail/Utf16ToUtf8.hpp>

#include <catch2/catch_test_macros.hpp>

#include <initializer_list>
#include <string>

using core::tui::detail::Utf16ToUtf8;

namespace
{

/// Feeds @p units through one converter, as one console read would.
std::string convert(std::initializer_list<char16_t> units)
{
    auto converter = Utf16ToUtf8 {};
    auto output = std::string {};
    for (auto const unit: units)
        converter.append(output, unit);
    return output;
}

} // namespace

TEST_CASE("Utf16ToUtf8.bmp_code_units_encode_at_every_width", "[tui,utf16]")
{
    CHECK(convert({ u'a' }) == "a");
    CHECK(convert({ u'é' }) == "\xC3\xA9");
    CHECK(convert({ u'€' }) == "\xE2\x82\xAC");
}

TEST_CASE("Utf16ToUtf8.a_surrogate_pair_is_one_four_byte_sequence_not_cesu8", "[tui,utf16]")
{
    // U+1F600, which a console delivers as two key events: 0xD83D, then 0xDE00. Encoded one code
    // unit at a time it was ED A0 BD ED B8 80 (core-cpp#20).
    CHECK(convert({ char16_t { 0xD83D }, char16_t { 0xDE00 } }) == "\xF0\x9F\x98\x80");
}

TEST_CASE("Utf16ToUtf8.a_pair_split_across_two_reads_is_still_paired", "[tui,utf16]")
{
    auto converter = Utf16ToUtf8 {};
    auto first = std::string {};
    converter.append(first, char16_t { 0xD83D });
    CHECK(first.empty());
    CHECK(converter.pending());

    auto second = std::string {};
    converter.append(second, char16_t { 0xDE00 });
    CHECK(second == "\xF0\x9F\x98\x80");
    CHECK_FALSE(converter.pending());
}

TEST_CASE("Utf16ToUtf8.an_unpaired_surrogate_becomes_the_replacement_character", "[tui,utf16]")
{
    // A low surrogate with nothing before it.
    CHECK(convert({ char16_t { 0xDE00 }, u'a' })
          == "\xEF\xBF\xBD"
             "a");
    // A high surrogate followed by something that is not its low half: the replacement, then that.
    CHECK(convert({ char16_t { 0xD83D }, u'a' })
          == "\xEF\xBF\xBD"
             "a");
    CHECK(convert({ char16_t { 0xD83D }, char16_t { 0xD83D }, char16_t { 0xDE00 } })
          == "\xEF\xBF\xBD"
             "\xF0\x9F\x98\x80");
}
