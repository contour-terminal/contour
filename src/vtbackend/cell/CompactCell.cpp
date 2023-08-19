// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/cell/CompactCell.h>

namespace terminal
{

std::u32string CompactCell::codepoints() const
{
    std::u32string s;
    if (_codepoint)
    {
        s += _codepoint;
        if (_extra)
        {
            for (char32_t const cp: _extra->codepoints)
            {
                s += cp;
            }
        }
    }
    return s;
}

std::string CompactCell::toUtf8() const
{
    if (!_codepoint)
        return {};

    std::string text;
    text += unicode::convert_to<char>(_codepoint);
    if (_extra)
        for (char32_t const cp: _extra->codepoints)
            text += unicode::convert_to<char>(cp);
    return text;
}

} // namespace terminal
