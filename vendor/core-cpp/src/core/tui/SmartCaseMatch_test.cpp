// SPDX-License-Identifier: Apache-2.0
#include <core/tui/completer/SmartCaseMatch.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace core::tui;
using namespace core::tui::completer;

// ============================================================================
// hasUppercase tests
// ============================================================================

TEST_CASE("SmartCaseMatch.hasUppercase_empty")
{
    CHECK_FALSE(SmartCaseMatch::hasUppercase(""));
}

TEST_CASE("SmartCaseMatch.hasUppercase_all_lowercase")
{
    CHECK_FALSE(SmartCaseMatch::hasUppercase("hello"));
    CHECK_FALSE(SmartCaseMatch::hasUppercase("foobar123"));
    CHECK_FALSE(SmartCaseMatch::hasUppercase("test_name"));
}

TEST_CASE("SmartCaseMatch.hasUppercase_with_uppercase")
{
    CHECK(SmartCaseMatch::hasUppercase("Hello"));
    CHECK(SmartCaseMatch::hasUppercase("helloWorld"));
    CHECK(SmartCaseMatch::hasUppercase("ALLCAPS"));
    CHECK(SmartCaseMatch::hasUppercase("mixedCase"));
    CHECK(SmartCaseMatch::hasUppercase("A"));
}

TEST_CASE("SmartCaseMatch.hasUppercase_numbers_symbols")
{
    CHECK_FALSE(SmartCaseMatch::hasUppercase("12345"));
    CHECK_FALSE(SmartCaseMatch::hasUppercase("!@#$%"));
    CHECK_FALSE(SmartCaseMatch::hasUppercase("_-./"));
}

// ============================================================================
// matchesPrefixCaseInsensitive tests
// ============================================================================

TEST_CASE("SmartCaseMatch.matchesPrefixCaseInsensitive_empty_pattern")
{
    CHECK(SmartCaseMatch::matchesPrefixCaseInsensitive("hello", ""));
    CHECK(SmartCaseMatch::matchesPrefixCaseInsensitive("", ""));
}

TEST_CASE("SmartCaseMatch.matchesPrefixCaseInsensitive_exact_match")
{
    CHECK(SmartCaseMatch::matchesPrefixCaseInsensitive("hello", "hello"));
    CHECK(SmartCaseMatch::matchesPrefixCaseInsensitive("hello", "HELLO"));
    CHECK(SmartCaseMatch::matchesPrefixCaseInsensitive("HELLO", "hello"));
}

TEST_CASE("SmartCaseMatch.matchesPrefixCaseInsensitive_prefix")
{
    CHECK(SmartCaseMatch::matchesPrefixCaseInsensitive("hello", "hel"));
    CHECK(SmartCaseMatch::matchesPrefixCaseInsensitive("Hello", "hel"));
    CHECK(SmartCaseMatch::matchesPrefixCaseInsensitive("HELLO", "hel"));
    CHECK(SmartCaseMatch::matchesPrefixCaseInsensitive("hello", "HEL"));
}

TEST_CASE("SmartCaseMatch.matchesPrefixCaseInsensitive_no_match")
{
    CHECK_FALSE(SmartCaseMatch::matchesPrefixCaseInsensitive("hello", "world"));
    CHECK_FALSE(SmartCaseMatch::matchesPrefixCaseInsensitive("hello", "helloo"));
}

// ============================================================================
// matchesPrefixCaseSensitive tests
// ============================================================================

TEST_CASE("SmartCaseMatch.matchesPrefixCaseSensitive_empty_pattern")
{
    CHECK(SmartCaseMatch::matchesPrefixCaseSensitive("hello", ""));
}

TEST_CASE("SmartCaseMatch.matchesPrefixCaseSensitive_exact_case")
{
    CHECK(SmartCaseMatch::matchesPrefixCaseSensitive("Hello", "Hel"));
    CHECK(SmartCaseMatch::matchesPrefixCaseSensitive("hello", "hel"));
    CHECK(SmartCaseMatch::matchesPrefixCaseSensitive("HELLO", "HEL"));
}

TEST_CASE("SmartCaseMatch.matchesPrefixCaseSensitive_wrong_case")
{
    CHECK_FALSE(SmartCaseMatch::matchesPrefixCaseSensitive("Hello", "hel"));
    CHECK_FALSE(SmartCaseMatch::matchesPrefixCaseSensitive("hello", "HEL"));
    CHECK_FALSE(SmartCaseMatch::matchesPrefixCaseSensitive("HELLO", "hel"));
}

// ============================================================================
// matchesPrefix (smart case) tests
// ============================================================================

TEST_CASE("SmartCaseMatch.matchesPrefix_lowercase_pattern_case_insensitive")
{
    // Lowercase pattern should match case-insensitively
    CHECK(SmartCaseMatch::matchesPrefix("FooBar", "foo"));
    CHECK(SmartCaseMatch::matchesPrefix("foobar", "foo"));
    CHECK(SmartCaseMatch::matchesPrefix("FOOBAR", "foo"));
    CHECK(SmartCaseMatch::matchesPrefix("FOO", "foo"));
}

TEST_CASE("SmartCaseMatch.matchesPrefix_uppercase_pattern_case_sensitive")
{
    // Pattern with uppercase should match case-sensitively
    CHECK(SmartCaseMatch::matchesPrefix("FooBar", "Foo"));
    CHECK_FALSE(SmartCaseMatch::matchesPrefix("foobar", "Foo"));
    CHECK_FALSE(SmartCaseMatch::matchesPrefix("FOOBAR", "Foo"));

    CHECK(SmartCaseMatch::matchesPrefix("FOOBAR", "FOO"));
    CHECK_FALSE(SmartCaseMatch::matchesPrefix("foobar", "FOO"));
}

TEST_CASE("SmartCaseMatch.matchesPrefix_empty_pattern")
{
    CHECK(SmartCaseMatch::matchesPrefix("anything", ""));
    CHECK(SmartCaseMatch::matchesPrefix("", ""));
}

TEST_CASE("SmartCaseMatch.matchesPrefix_pattern_longer_than_text")
{
    CHECK_FALSE(SmartCaseMatch::matchesPrefix("hi", "hello"));
}

TEST_CASE("SmartCaseMatch.matchesPrefix_single_uppercase")
{
    // Even a single uppercase makes it case-sensitive
    CHECK(SmartCaseMatch::matchesPrefix("Hello", "H"));
    CHECK_FALSE(SmartCaseMatch::matchesPrefix("hello", "H"));
}

TEST_CASE("SmartCaseMatch.matchesPrefix_mixed_case_pattern")
{
    CHECK(SmartCaseMatch::matchesPrefix("getString", "getS"));
    CHECK_FALSE(SmartCaseMatch::matchesPrefix("getstring", "getS"));
    CHECK_FALSE(SmartCaseMatch::matchesPrefix("GetString", "getS"));
}

// ============================================================================
// matchesExact tests
// ============================================================================

TEST_CASE("SmartCaseMatch.matchesExact_lowercase_pattern")
{
    CHECK(SmartCaseMatch::matchesExact("hello", "hello"));
    CHECK(SmartCaseMatch::matchesExact("HELLO", "hello"));
    CHECK(SmartCaseMatch::matchesExact("Hello", "hello"));
}

TEST_CASE("SmartCaseMatch.matchesExact_uppercase_pattern")
{
    CHECK(SmartCaseMatch::matchesExact("Hello", "Hello"));
    CHECK_FALSE(SmartCaseMatch::matchesExact("hello", "Hello"));
    CHECK_FALSE(SmartCaseMatch::matchesExact("HELLO", "Hello"));
}

TEST_CASE("SmartCaseMatch.matchesExact_length_mismatch")
{
    CHECK_FALSE(SmartCaseMatch::matchesExact("hello", "hel"));
    CHECK_FALSE(SmartCaseMatch::matchesExact("hel", "hello"));
}

// ============================================================================
// adjustScore tests
// ============================================================================

TEST_CASE("SmartCaseMatch.adjustScore_exact_match_bonus")
{
    SmartCaseConfig const config { .exactMatchBonus = 50, .caseExactPrefixBonus = 25 };

    // Exact case-sensitive match gets exactMatchBonus
    int const score = SmartCaseMatch::adjustScore(100, "hello", "hello", config);
    CHECK(score == 150);
}

TEST_CASE("SmartCaseMatch.adjustScore_case_exact_prefix_bonus")
{
    SmartCaseConfig const config { .exactMatchBonus = 50, .caseExactPrefixBonus = 25 };

    // Case-exact prefix gets caseExactPrefixBonus
    int const score = SmartCaseMatch::adjustScore(100, "hello_world", "hello", config);
    CHECK(score == 125);
}

TEST_CASE("SmartCaseMatch.adjustScore_no_bonus_case_insensitive")
{
    SmartCaseConfig const config { .exactMatchBonus = 50, .caseExactPrefixBonus = 25 };

    // Case-insensitive match (case differs) gets no bonus
    int score = SmartCaseMatch::adjustScore(100, "Hello", "hello", config);
    CHECK(score == 100);

    score = SmartCaseMatch::adjustScore(100, "HELLO", "hello", config);
    CHECK(score == 100);
}

TEST_CASE("SmartCaseMatch.adjustScore_default_config")
{
    // Using default config
    int const score = SmartCaseMatch::adjustScore(100, "hello", "hello");
    CHECK(score == 150); // Default exactMatchBonus is 50
}

TEST_CASE("SmartCaseMatch.adjustScore_empty_pattern")
{
    SmartCaseConfig const config { .exactMatchBonus = 50, .caseExactPrefixBonus = 25 };

    // Empty pattern is a case-exact prefix of any string
    int const score = SmartCaseMatch::adjustScore(100, "hello", "", config);
    CHECK(score == 125);
}

TEST_CASE("SmartCaseMatch.adjustScore_custom_config")
{
    SmartCaseConfig const config { .exactMatchBonus = 100, .caseExactPrefixBonus = 10 };

    int const score1 = SmartCaseMatch::adjustScore(50, "cat", "cat", config);
    CHECK(score1 == 150); // 50 + 100

    int const score2 = SmartCaseMatch::adjustScore(50, "catalog", "cat", config);
    CHECK(score2 == 60); // 50 + 10
}

TEST_CASE("SmartCaseMatch.adjustScore_zero_bonuses")
{
    SmartCaseConfig const config { .exactMatchBonus = 0, .caseExactPrefixBonus = 0 };

    int const score = SmartCaseMatch::adjustScore(100, "hello", "hello", config);
    CHECK(score == 100);
}

// ============================================================================
// Edge cases
// ============================================================================

TEST_CASE("SmartCaseMatch.edge_case_numbers_in_pattern")
{
    // Numbers don't affect case sensitivity
    CHECK(SmartCaseMatch::matchesPrefix("file123", "file"));
    CHECK(SmartCaseMatch::matchesPrefix("File123", "file"));
    CHECK(SmartCaseMatch::matchesPrefix("file123", "file1"));
}

TEST_CASE("SmartCaseMatch.edge_case_underscore_pattern")
{
    CHECK(SmartCaseMatch::matchesPrefix("my_function", "my_"));
    CHECK(SmartCaseMatch::matchesPrefix("MY_FUNCTION", "my_"));
    CHECK(SmartCaseMatch::matchesPrefix("My_Function", "My_"));
    CHECK_FALSE(SmartCaseMatch::matchesPrefix("my_function", "My_"));
}

TEST_CASE("SmartCaseMatch.edge_case_camelCase")
{
    // Typical programming identifier patterns
    CHECK(SmartCaseMatch::matchesPrefix("getElementById", "get"));
    CHECK(SmartCaseMatch::matchesPrefix("GetElementById", "get"));

    CHECK(SmartCaseMatch::matchesPrefix("getElementById", "getElement"));
    CHECK_FALSE(SmartCaseMatch::matchesPrefix("GetElementById", "getElement"));

    CHECK(SmartCaseMatch::matchesPrefix("GetElementById", "GetElement"));
    CHECK_FALSE(SmartCaseMatch::matchesPrefix("getElementById", "GetElement"));
}

TEST_CASE("SmartCaseMatch.edge_case_path_like")
{
    CHECK(SmartCaseMatch::matchesPrefix("/home/user/Documents", "/home/user"));
    CHECK(SmartCaseMatch::matchesPrefix("/Home/User/Documents", "/home"));
    CHECK_FALSE(SmartCaseMatch::matchesPrefix("/home/user/Documents", "/Home"));
}
