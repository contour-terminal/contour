// SPDX-License-Identifier: Apache-2.0
#include <text_shaper/DirectWriteLocator.hpp>
#include <text_shaper/DirectWriteShaper.hpp>
#include <text_shaper/FontLocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

using namespace text;

namespace
{

/// Bytes that are not UTF-8: a stray continuation byte, then one that starts no sequence at all.
constexpr auto NotUtf8 = std::string_view { "\x80\xff" };

/// Answers every query with the one file it was given, so a shaper loads the path a case chose.
class OneFileLocator final: public FontLocator
{
  public:
    /// @param path The font file every query answers with.
    explicit OneFileLocator(std::string path): _path { std::move(path) } {}

    [[nodiscard]] FontSourceList all() override { return { FontPath { .value = _path } }; }

    [[nodiscard]] FontSourceList locate(FontDescription const& /*description*/) override { return all(); }

    [[nodiscard]] FontSourceList resolve(gsl::span<char32_t const> /*codepoints*/) override { return all(); }

  private:
    std::string _path;
};

} // namespace

TEST_CASE("DirectWriteLocator.locate")
{
    auto locator = DirectWriteLocator {};

    SECTION("a family name that is not UTF-8 falls back to the default family")
    {
        auto const description = FontDescription { .familyName = std::string { NotUtf8 } };
        auto sources = FontSourceList {};
        REQUIRE_NOTHROW(sources = locator.locate(description));
        CHECK_FALSE(sources.empty());
    }
}

TEST_CASE("DirectWriteLocator.resolve")
{
    auto locator = DirectWriteLocator {};

    SECTION("a codepoint beyond the BMP maps to a font")
    {
        // Two UTF-16 code units: a surrogate split wrongly maps nothing, or maps something else.
        auto const grinningFace = std::array { char32_t { 0x1F600 } };
        CHECK_FALSE(locator.resolve(grinningFace).empty());
    }
}

TEST_CASE("DirectWriteShaper.loadFont")
{
    SECTION("a font path that is not UTF-8 loads no font")
    {
        auto locator = OneFileLocator { std::string { NotUtf8 } };
        auto shaper = DirectWriteShaper { DPI { .x = 96, .y = 96 }, locator };
        auto font = std::optional<FontKey> {};
        REQUIRE_NOTHROW(font = shaper.loadFont(FontDescription {}, FontSize { 12.0 }));
        CHECK_FALSE(font.has_value());
    }
}
