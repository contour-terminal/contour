// SPDX-License-Identifier: Apache-2.0
#include <text_shaper/LastResortFontLocator.hpp>

#include <format>
#include <string_view>
#include <utility>

namespace text
{

namespace
{
    /// The generic family the second step asks for; each platform locator maps it to its own.
    constexpr auto MonospaceFamily = std::string_view { "monospace" };
} // namespace

LastResortFontLocator::LastResortFontLocator(FontLocator& inner): _inner { inner }
{
}

CoverageFallbackSpacing LastResortFontLocator::coverageFallbackSpacing() const noexcept
{
    return _inner.coverageFallbackSpacing();
}

FontSourceList LastResortFontLocator::all()
{
    return _inner.all();
}

FontSourceList LastResortFontLocator::locate(FontDescription const& description)
{
    if (auto found = _inner.locate(description); !found.empty())
        return found;

    if (description.familyName != MonospaceFamily)
    {
        auto monospace = description;
        monospace.familyName = MonospaceFamily;
        monospace.spacing = FontSpacing::Mono;
        monospace.strictSpacing = false;
        if (auto found = _inner.locate(monospace); !found.empty())
        {
            locatorLog()("Font family \"{}\" not found; using a monospace font instead: {}",
                         description.familyName,
                         found.front());
            return found;
        }
    }

    auto everything = _inner.all();
    if (everything.empty())
        return {};
    locatorLog()(
        "Font family \"{}\" not found, nor any monospace font; using the first font found instead: {}",
        description.familyName,
        everything.front());
    return FontSourceList { std::move(everything.front()) };
}

FontSourceList LastResortFontLocator::resolve(gsl::span<char32_t const> codepoints)
{
    return _inner.resolve(codepoints);
}

std::expected<FontSource, FontUnavailable> locatePrimaryFont(FontLocator& locator,
                                                             FontDescription const& description)
{
    auto found = locator.locate(description);
    if (found.empty())
        return std::unexpected { FontUnavailable::NoFontInstalled };
    return std::move(found.front());
}

std::string describe(FontUnavailable reason, FontDescription const& description)
{
    switch (reason)
    {
        case FontUnavailable::NoFontInstalled:
            return std::format(
                "No usable font was found: neither the configured font \"{}\", nor any "
                "monospace font, nor any other font is installed. Install a monospace font "
                "(for example Cascadia Mono, Consolas or JetBrains Mono) and start Contour again.",
                description.familyName);
    }
    return "No usable font was found.";
}

} // namespace text
