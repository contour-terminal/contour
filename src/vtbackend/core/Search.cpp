// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/core/Search.hpp>

#include <libunicode/ucd.h>

#include <algorithm>

namespace vtbackend
{

bool isCaseSensitiveSearch(std::u32string_view searchText, SearchCaseSensitivity mode) noexcept
{
    switch (mode)
    {
        case SearchCaseSensitivity::Insensitive: return false;
        case SearchCaseSensitivity::Sensitive: return true;
        case SearchCaseSensitivity::Smart: break;
    }

    // NB: std::isupper() takes an *int holding an unsigned char value* (or EOF), so feeding it a
    // char32_t is undefined for every codepoint above 0xFF -- and it would answer for the wrong
    // alphabet anyway. The UCD lookup is the codepoint-correct test, so "Привет" and "Ünicode"
    // select a case-sensitive search just like "Hello" does.
    return std::ranges::any_of(searchText, unicode::general_category::is_uppercase_letter);
}

} // namespace vtbackend
