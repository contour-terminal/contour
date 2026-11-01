// SPDX-License-Identifier: Apache-2.0
#include <text_shaper/NerdFontSymbolsLocator.hpp>

#include <text_shaper/NerdFontSymbolRanges.hpp>

#include <core/Utf8Path.hpp>

#include <algorithm>
#include <system_error>

namespace text
{

NerdFontSymbolsLocator::NerdFontSymbolsLocator(FontLocator& inner, std::filesystem::path const& symbolsFont):
    _inner { inner }
{
    auto error = std::error_code {};
    if (std::filesystem::is_regular_file(symbolsFont, error))
        _symbols = FontPath { .value = core::pathToUtf8(symbolsFont) };
}

CoverageFallbackSpacing NerdFontSymbolsLocator::coverageFallbackSpacing() const noexcept
{
    return _inner.coverageFallbackSpacing();
}

FontSourceList NerdFontSymbolsLocator::all()
{
    return _inner.all();
}

FontSourceList NerdFontSymbolsLocator::locate(FontDescription const& description)
{
    return _inner.locate(description);
}

FontSourceList NerdFontSymbolsLocator::resolve(gsl::span<char32_t const> codepoints)
{
    if (_symbols && !codepoints.empty() && std::ranges::all_of(codepoints, isNerdFontSymbol))
        return FontSourceList { *_symbols };
    return _inner.resolve(codepoints);
}

} // namespace text
