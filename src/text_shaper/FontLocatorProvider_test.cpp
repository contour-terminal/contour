// SPDX-License-Identifier: Apache-2.0
#include <text_shaper/BundledFonts.hpp>
#include <text_shaper/FontLocatorProvider.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <variant>

using namespace text;

namespace
{
FontDescription description(std::string family)
{
    auto result = FontDescription {};
    result.familyName = std::move(family);
    result.fontFallback = FontFallbackNone {};
    return result;
}

std::filesystem::path bundledFontsSourceDir()
{
    return std::filesystem::path { CONTOUR_BUNDLED_FONTS_SOURCE_DIR };
}
} // namespace

TEST_CASE("FontLocator.registerBundledFonts reports a missing directory and keeps lookup working",
          "[bundled-fonts]")
{
    auto const missing = bundledFontsSourceDir() / "does-not-exist";
    auto const result = registerBundledFonts(BundledFontDirectories { .paths = { missing } });
    REQUIRE(!result);
    CHECK(result.error() == FontRegistrationError::DirectoryMissing);

    auto& locator = FontLocatorProvider::get().native();
    CHECK(!locator.locate(description("monospace")).empty());
}

TEST_CASE("FontLocator.listBundledFontFiles finds the shipped fonts", "[bundled-fonts]")
{
    auto const files = listBundledFontFiles(bundledFontsSourceDir());
    REQUIRE(files);
    CHECK(files->size() == 2);
    CHECK(!listBundledFontFiles(bundledFontsSourceDir() / "nope"));
}

#if !defined(_WIN32)
TEST_CASE("FontLocator.registerBundledFonts makes JetBrains Mono resolvable", "[bundled-fonts]")
{
    REQUIRE(registerBundledFonts(BundledFontDirectories { .paths = { bundledFontsSourceDir() } }));
    // Registering twice is harmless.
    REQUIRE(registerBundledFonts(BundledFontDirectories { .paths = { bundledFontsSourceDir() } }));

    auto& locator = FontLocatorProvider::get().native();
    auto const fonts = locator.locate(description("JetBrains Mono"));
    REQUIRE(!fonts.empty());
    auto const& path = std::get<FontPath>(fonts.front());
    CHECK(path.value.ends_with("JetBrainsMono[wght].ttf"));
}
#endif
