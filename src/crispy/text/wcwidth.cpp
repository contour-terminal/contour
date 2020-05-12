/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2020 Christian Parpart <christian@parpart.family>
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

#include <crispy/text/wcwidth.h>
#include <crispy/text/Unicode.h>

#if defined(UTF8PROC_FOUND) && UTF8PROC_FOUND
#include <utf8proc.h>
#endif

namespace crispy::text {

int wcwidth(char32_t _codepoint) noexcept
{
    if (emoji(_codepoint) && !emoji_component(_codepoint))
        return 2;

#if defined(UTF8PROC_FOUND) && UTF8PROC_FOUND
    auto const cat = utf8proc_category(_codepoint);
    if (cat != UTF8PROC_CATEGORY_CO)
        return utf8proc_charwidth(_codepoint);
    else
        return 1; // private category is treated as standard column count
#else
    return ::wcwidth(_codepoint);
#endif
}

int mbtowc(char32_t* wc, char const* s, size_t n) noexcept
{
#if defined(UTF8PROC_FOUND) && UTF8PROC_FOUND
    if (wc)
    {
        auto const slen = utf8proc_iterate(reinterpret_cast<utf8proc_uint8_t const*>(s),
                                           static_cast<utf8proc_ssize_t>(n),
                                           reinterpret_cast<utf8proc_int32_t*>(wc));
        if (slen >= 0 && static_cast<int>(*wc) != -1)
            return slen;
    }
    return -1;
#else
    return ::mbtowc(reinterpret_cast<wchar_t*>(wc), s, n);
#endif
}

} // namespace crispy::text
