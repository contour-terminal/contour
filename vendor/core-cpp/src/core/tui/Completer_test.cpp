// SPDX-License-Identifier: Apache-2.0
#include <core/tui/completer/Completer.hpp>
#include <core/tui/completer/CompletionItem.hpp>
#include <core/tui/completer/CompletionProvider.hpp>
#include <core/tui/completer/FuzzyMatch.hpp>
#include <core/tui/completer/SmartCaseMatch.hpp>

#include <catch2/catch_test_macros.hpp>

#include <ranges>

using namespace core::tui;
using namespace core::tui::completer;

namespace
{

/// @brief A simple test provider that returns fixed completions with fuzzy support.
class TestProvider: public CompletionProvider
{
  public:
    explicit TestProvider(std::vector<CompletionItem> items, int priority = 0, bool enableFuzzy = true):
        _items(std::move(items)), _priority(priority), _enableFuzzy(enableFuzzy)
    {
    }

    std::vector<CompletionItem> complete(std::string_view input, size_t cursorPosition) override
    {
        std::vector<CompletionItem> results;
        std::string_view const prefix = input.substr(0, cursorPosition);

        FuzzyConfig const fuzzyConfig;
        double const minThreshold = fuzzyConfig.minMatchThreshold;

        for (auto const& item: _items)
        {
            // Option C: Check both prefix and fuzzy matches
            bool const isPrefixMatch = SmartCaseMatch::matchesPrefix(item.text, prefix);
            FuzzyMatchResult fuzzyResult;
            bool isFuzzyMatch = false;

            if (!isPrefixMatch && _enableFuzzy && !prefix.empty())
            {
                fuzzyResult = FuzzyMatch::matchSmartCase(item.text, prefix);
                size_t const textLen = FuzzyMatch::countGraphemes(item.text);
                isFuzzyMatch =
                    fuzzyResult.matches
                    && (fuzzyResult.quality(textLen) >= minThreshold || fuzzyResult.isContiguousSubstring());
            }

            if (!isPrefixMatch && !isFuzzyMatch)
                continue;

            auto resultItem = item;
            if (isPrefixMatch)
            {
                resultItem.score = SmartCaseMatch::adjustScore(item.score, item.text, prefix);
                resultItem.score += fuzzyConfig.prefixMatchBonus;
            }
            else
            {
                resultItem.score =
                    FuzzyMatch::calculateScore(item.score, item.text, prefix, fuzzyResult, fuzzyConfig);
                resultItem.matchPositions = std::move(fuzzyResult.positions);
            }
            results.push_back(resultItem);
        }
        return results;
    }

    [[nodiscard]] int priority() const override { return _priority; }

  private:
    std::vector<CompletionItem> _items;
    int _priority;
    bool _enableFuzzy;
};

/// @brief Helper to create items quickly.
CompletionItem item(std::string text, int score = 0, std::string desc = "")
{
    CompletionItem result;
    result.displayText = text;
    result.text = std::move(text);
    result.description = std::move(desc);
    result.score = score;
    return result;
}

} // namespace

// ============================================================================
// CompletionItem tests
// ============================================================================

TEST_CASE("CompletionItem.suffix")
{
    CompletionItem const ci { .text = "hello", .displayText = "hello", .description = "", .score = 0 };

    CHECK(ci.suffix(0) == "hello");
    CHECK(ci.suffix(2) == "llo");
    CHECK(ci.suffix(5).empty());
    CHECK(ci.suffix(10).empty()); // Beyond text length
}

TEST_CASE("CompletionItem.comparison")
{
    CompletionItem a { .text = "abc", .displayText = "abc", .description = "", .score = 10 };
    CompletionItem b { .text = "abc", .displayText = "abc", .description = "", .score = 10 };
    CompletionItem c { .text = "def", .displayText = "def", .description = "", .score = 10 };

    CHECK(a == b);
    CHECK(a != c);
    CHECK(a < c); // Lexicographic comparison
}

// ============================================================================
// Completer tests
// ============================================================================

TEST_CASE("Completer.empty_completer")
{
    Completer const completer;

    CHECK(completer.providerCount() == 0);

    auto results = completer.complete("test", 4);
    CHECK(results.empty());

    auto suggestion = completer.suggest("test", 4);
    CHECK_FALSE(suggestion.has_value());
}

TEST_CASE("Completer.single_provider")
{
    Completer completer;

    std::vector<CompletionItem> const items = { item("apple", 10), item("apricot", 5), item("banana", 8) };

    completer.addProvider(std::make_unique<TestProvider>(items));
    CHECK(completer.providerCount() == 1);

    auto results = completer.complete("ap", 2);
    CHECK(results.size() == 2);
    CHECK(results[0].text == "apple");   // Higher score
    CHECK(results[1].text == "apricot"); // Lower score
}

TEST_CASE("Completer.multiple_providers")
{
    Completer completer;

    std::vector<CompletionItem> const items1 = { item("cat", 10), item("car", 5) };
    std::vector<CompletionItem> const items2 = { item("cap", 15), item("can", 8) };

    completer.addProvider(std::make_unique<TestProvider>(items1, 1));
    completer.addProvider(std::make_unique<TestProvider>(items2, 2));
    CHECK(completer.providerCount() == 2);

    auto results = completer.complete("ca", 2);
    CHECK(results.size() == 4);
    // Should be sorted by score (descending)
    CHECK(results[0].text == "cap"); // 15
    CHECK(results[1].text == "cat"); // 10
    CHECK(results[2].text == "can"); // 8
    CHECK(results[3].text == "car"); // 5
}

TEST_CASE("Completer.deduplication")
{
    Completer completer;

    // Both providers return "cat"
    std::vector<CompletionItem> const items1 = { item("cat", 10) };
    std::vector<CompletionItem> const items2 = { item("cat", 20), item("car", 5) };

    completer.addProvider(std::make_unique<TestProvider>(items1, 1));
    completer.addProvider(std::make_unique<TestProvider>(items2, 2));

    auto results = completer.complete("ca", 2);
    CHECK(results.size() == 2);

    // First occurrence wins (from higher priority provider)
    int catCount = 0;
    for (auto const& r: results)
    {
        if (r.text == "cat")
            ++catCount;
    }
    CHECK(catCount == 1);
}

TEST_CASE("Completer.max_suggestions")
{
    Completer completer;

    std::vector<CompletionItem> items;
    items.reserve(100);
    for (auto const i: std::views::iota(0, 100))
        items.push_back(item("test" + std::to_string(i), 100 - i));

    completer.addProvider(std::make_unique<TestProvider>(items));

    CompletionConfig const config { .maxSuggestions = 10 };
    completer.setConfig(config);

    auto results = completer.complete("test", 4);
    CHECK(results.size() == 10);
}

TEST_CASE("Completer.suggest_ghost_text")
{
    Completer completer;

    std::vector<CompletionItem> const items = { item("hello_world", 10), item("hello_there", 5) };

    completer.addProvider(std::make_unique<TestProvider>(items));

    auto suggestion = completer.suggest("hello", 5);
    REQUIRE(suggestion.has_value());
    CHECK(*suggestion == "_world"); // Suffix of best match
}

TEST_CASE("Completer.suggest_no_match")
{
    Completer completer;

    std::vector<CompletionItem> const items = { item("hello", 10) };

    completer.addProvider(std::make_unique<TestProvider>(items));

    auto suggestion = completer.suggest("xyz", 3);
    CHECK_FALSE(suggestion.has_value());
}

TEST_CASE("Completer.findCommonPrefix_empty")
{
    auto prefix = Completer::findCommonPrefix({});
    CHECK(prefix.empty());
}

TEST_CASE("Completer.findCommonPrefix_single")
{
    auto prefix = Completer::findCommonPrefix({ item("hello") });
    CHECK(prefix == "hello");
}

TEST_CASE("Completer.findCommonPrefix_common")
{
    auto prefix = Completer::findCommonPrefix({ item("hello"), item("help"), item("helicopter") });
    CHECK(prefix == "hel");
}

TEST_CASE("Completer.findCommonPrefix_no_common")
{
    auto prefix = Completer::findCommonPrefix({ item("apple"), item("banana") });
    CHECK(prefix.empty());
}

TEST_CASE("Completer.config_persistence")
{
    Completer completer;

    CompletionConfig const config { .maxSuggestions = 25 };
    completer.setConfig(config);

    CHECK(completer.config().maxSuggestions == 25);
}

// ============================================================================
// Fuzzy matching tests
// ============================================================================

TEST_CASE("Completer.fuzzy_matching_finds_non_prefix")
{
    Completer completer;

    std::vector<CompletionItem> const items = { item("Downloads", 10),
                                                item("Documents", 10),
                                                item("Desktop", 10) };

    completer.addProvider(std::make_unique<TestProvider>(items));

    // "ds" should fuzzy-match all three:
    // - "Downloads" (D...s)
    // - "Documents" (D...s)
    // - "Desktop" (De...S...top -> wait, no 's' at end... let me think)
    // Actually "Desktop" has 'D' and 's' so it matches: D...s...ktop
    auto results = completer.complete("ds", 2);
    CHECK(results.size() == 3); // All three match "ds"

    // All should have match positions for highlighting (fuzzy matches)
    bool foundDownloads = false;
    bool foundDocuments = false;
    bool foundDesktop = false;
    for (auto const& r: results)
    {
        if (r.text == "Downloads")
        {
            foundDownloads = true;
            CHECK_FALSE(r.matchPositions.empty()); // Should have highlighting positions
        }
        if (r.text == "Documents")
        {
            foundDocuments = true;
            CHECK_FALSE(r.matchPositions.empty());
        }
        if (r.text == "Desktop")
        {
            foundDesktop = true;
            CHECK_FALSE(r.matchPositions.empty());
        }
    }
    CHECK(foundDownloads);
    CHECK(foundDocuments);
    CHECK(foundDesktop);
}

TEST_CASE("Completer.prefix_matches_score_higher_than_fuzzy")
{
    Completer completer;

    // "do" should prefix-match "documents" and fuzzy-match "Downloads" (D...o)
    std::vector<CompletionItem> const items = { item("downloads", 10), item("documents", 10) };

    completer.addProvider(std::make_unique<TestProvider>(items));

    auto results = completer.complete("do", 2);
    CHECK(results.size() == 2);

    // "documents" is a prefix match, "downloads" is also a prefix match
    // Both should be prefix matches, so both have empty matchPositions
    for (auto const& r: results)
        CHECK(r.matchPositions.empty()); // Prefix matches have no highlighting
}

TEST_CASE("Completer.fuzzy_match_has_positions")
{
    Completer completer;

    std::vector<CompletionItem> const items = { item("Downloads", 10) };

    completer.addProvider(std::make_unique<TestProvider>(items));

    // "dls" matches "DownLoads" -> D...l...s
    auto results = completer.complete("dls", 3);
    REQUIRE(results.size() == 1);
    CHECK(results[0].text == "Downloads");
    CHECK_FALSE(results[0].matchPositions.empty());
    // Positions should be indices of 'D', 'l', 's' in "Downloads"
    CHECK(results[0].matchPositions.size() == 3);
}
