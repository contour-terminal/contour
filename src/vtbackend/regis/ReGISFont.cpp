// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/regis/ReGISFont.h>

namespace vtbackend::regis
{

Glyph const& ReGISBitmapFont::glyphOf(char ch) const noexcept
{
    auto const code = static_cast<unsigned char>(ch);
    auto const inRange = code >= 0x20 && code <= 0x7E;
    auto const index = inRange ? static_cast<size_t>(code - 0x20) : size_t { 0 };

    // A down-loadable alphabet (the L command) shadows the base font when loaded; none are yet.
    if (auto const& alphabet = _userAlphabets[0]; alphabet.has_value() && inRange)
        return (*alphabet)[index];

    return BasicFont[index];
}

} // namespace vtbackend::regis
