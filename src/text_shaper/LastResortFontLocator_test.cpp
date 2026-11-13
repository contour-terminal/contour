// SPDX-License-Identifier: Apache-2.0
#include <text_shaper/LastResortFontLocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace text;

namespace
{

/// A locator that knows the families it is given and nothing else, and remembers what it was asked: the
/// stand-in for a system whose fonts the case chooses -- one without the configured family, or with no
/// fonts at all, as a fresh Windows prefix without Cascadia Mono or Consolas is.
class ScriptedLocator final: public FontLocator
{
  public:
    ScriptedLocator(std::map<std::string, FontSourceList> families, FontSourceList everything):
        _families { std::move(families) }, _everything { std::move(everything) }
    {
    }

    [[nodiscard]] FontSourceList all() override
    {
        ++allCalls;
        return _everything;
    }

    [[nodiscard]] FontSourceList locate(FontDescription const& description) override
    {
        asked.push_back(description);
        if (auto const found = _families.find(description.familyName); found != _families.end())
            return found->second;
        return {};
    }

    [[nodiscard]] FontSourceList resolve(gsl::span<char32_t const> /*codepoints*/) override { return {}; }

    std::vector<FontDescription> asked; ///< Every description locate() was asked for, in order.
    int allCalls = 0;                   ///< How often all() was asked.

  private:
    std::map<std::string, FontSourceList> _families;
    FontSourceList _everything;
};

[[nodiscard]] FontSourceList fontAt(std::string path)
{
    return FontSourceList { FontPath { .value = std::move(path) } };
}

[[nodiscard]] FontDescription configured()
{
    auto description = FontDescription {};
    description.familyName = "Cascadia Mono";
    description.weight = FontWeight::Bold;
    description.spacing = FontSpacing::Mono;
    description.strictSpacing = true;
    return description;
}

[[nodiscard]] std::string pathOf(FontSource const& source)
{
    return std::get<FontPath>(source).value;
}

} // namespace

TEST_CASE("LastResortFontLocator answers with the configured family when there is one", "[fontlocator]")
{
    auto inner = ScriptedLocator { { { "Cascadia Mono", fontAt("cascadia.ttf") } }, fontAt("any.ttf") };
    auto locator = LastResortFontLocator { inner };

    auto const found = locator.locate(configured());
    REQUIRE(found.size() == 1);
    CHECK(pathOf(found.front()) == "cascadia.ttf");
    CHECK(inner.asked.size() == 1); // nothing more is asked of a system that has it
    CHECK(inner.allCalls == 0);
}

TEST_CASE("LastResortFontLocator falls back to any monospace font", "[fontlocator]")
{
    auto inner = ScriptedLocator { { { "monospace", fontAt("mono.ttf") } }, fontAt("any.ttf") };
    auto locator = LastResortFontLocator { inner };

    auto const found = locator.locate(configured());
    REQUIRE(found.size() == 1);
    CHECK(pathOf(found.front()) == "mono.ttf");
    REQUIRE(inner.asked.size() == 2);
    // The generic family, in the configured weight and slant, monospace but not insisting on it.
    CHECK(inner.asked[1].familyName == "monospace");
    CHECK(inner.asked[1].weight == FontWeight::Bold);
    CHECK(inner.asked[1].spacing == FontSpacing::Mono);
    CHECK_FALSE(inner.asked[1].strictSpacing);
    CHECK(inner.allCalls == 0);
}

TEST_CASE("LastResortFontLocator falls back to any font at all", "[fontlocator]")
{
    auto everything = fontAt("first.ttf");
    everything.emplace_back(FontPath { .value = "second.ttf" });
    auto inner = ScriptedLocator { {}, everything };
    auto locator = LastResortFontLocator { inner };

    auto const found = locator.locate(configured());
    // The first of what the system lists, which a locator orders fixed-pitch faces first; one font, as a
    // whole system's list makes no fallback chain.
    REQUIRE(found.size() == 1);
    CHECK(pathOf(found.front()) == "first.ttf");
    CHECK(inner.allCalls == 1);
}

TEST_CASE("LastResortFontLocator answers nothing on a system without fonts", "[fontlocator]")
{
    auto inner = ScriptedLocator { {}, {} };
    auto locator = LastResortFontLocator { inner };

    CHECK(locator.locate(configured()).empty());
    auto const primary = locatePrimaryFont(locator, configured());
    REQUIRE_FALSE(primary.has_value());
    CHECK(primary.error() == FontUnavailable::NoFontInstalled);
    CHECK(describe(primary.error(), configured()).contains("Cascadia Mono"));
}

TEST_CASE("locatePrimaryFont names the font that will be used", "[fontlocator]")
{
    auto inner = ScriptedLocator { { { "monospace", fontAt("mono.ttf") } }, {} };
    auto locator = LastResortFontLocator { inner };

    auto const primary = locatePrimaryFont(locator, configured());
    REQUIRE(primary.has_value());
    CHECK(pathOf(*primary) == "mono.ttf");
}

TEST_CASE("LastResortFontLocator leaves an explicit generic request alone", "[fontlocator]")
{
    // A request that already was for the generic family is not asked twice.
    auto inner = ScriptedLocator { {}, fontAt("any.ttf") };
    auto locator = LastResortFontLocator { inner };
    auto description = configured();
    description.familyName = "monospace";

    auto const found = locator.locate(description);
    REQUIRE(found.size() == 1);
    CHECK(pathOf(found.front()) == "any.ttf");
    CHECK(inner.asked.size() == 1);
}
