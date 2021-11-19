/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <terminal/Cell.h>

namespace terminal {

std::u32string Cell::codepoints() const
{
    std::u32string s;
    if (auto const base = codepoint(0); base)
    {
        s += base;
        if (extra_)
        {
            for (char32_t const cp: extra_->codepoints)
            {
                s += cp;
            }
        }
    }
    return s;
}

std::string Cell::toUtf8() const
{
    if (!codepoint_)
        return {};

    std::string text;
    text += unicode::convert_to<char>(codepoint_);
    if (extra_)
        for (char32_t const cp: extra_->codepoints)
            text += unicode::convert_to<char>(cp);
    return text;
}

GraphicsAttributes Cell::attributes() const noexcept
{
    GraphicsAttributes sgr{};
    sgr.foregroundColor = foregroundColor();
    sgr.backgroundColor = backgroundColor();
    sgr.underlineColor = underlineColor();
    sgr.styles = styles();
    return sgr;
}

}
