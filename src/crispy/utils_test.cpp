// SPDX-License-Identifier: Apache-2.0
#include <crispy/utils.h>

#include <catch2/catch_test_macros.hpp>

#include <map>

using std::string;
using std::string_view;
using namespace std::string_view_literals;

TEST_CASE("utils.split.0")
{
    auto result = crispy::splitKeyValuePairs("", ':');
    CHECK(result.empty());
}

TEST_CASE("utils.split.1")
{
    auto result = crispy::splitKeyValuePairs("foo=bar", ':');
    CHECK(result.size() == 1);
    CHECK(result["foo"] == "bar");

    auto result2 = crispy::splitKeyValuePairs("foo=bar::", ':');
    CHECK(result2.size() == 1);
    CHECK(result2["foo"] == "bar");

    auto result3 = crispy::splitKeyValuePairs("::foo=bar", ':');
    CHECK(result3.size() == 1);
    CHECK(result3["foo"] == "bar");
}

TEST_CASE("utils.split.2")
{
    auto result = crispy::splitKeyValuePairs("foo=bar:fnord=tar", ':');
    CHECK(result.size() == 2);
    CHECK(result["foo"] == "bar");
    CHECK(result["fnord"] == "tar");

    auto result2 = crispy::splitKeyValuePairs("foo=bar::fnord=tar", ':');
    CHECK(result2["foo"] == "bar");
    CHECK(result2["fnord"] == "tar");
    CHECK(result2.size() == 2);
}

template <typename R, typename... A>
R ret(R (*)(A...));
template <typename C, typename R, typename... A>
R ret(R (C::*)(A...));

TEST_CASE("utils.to_integer.integer_type")
{
    static_assert(
        std::is_same_v<uint8_t, std::remove_reference_t<decltype(*crispy::to_integer<10, uint8_t>(""sv))>>);

    static_assert(std::is_same_v<int, std::remove_reference_t<decltype(*crispy::to_integer<10, int>(""sv))>>);

    static_assert(
        std::is_same_v<unsigned, std::remove_reference_t<decltype(*crispy::to_integer<10, unsigned>(""sv))>>);

    static_assert(
        std::is_same_v<uint64_t, std::remove_reference_t<decltype(*crispy::to_integer<10, uint64_t>(""sv))>>);
}

TEST_CASE("utils.to_integer.bad")
{
    CHECK(crispy::to_integer<10>(""sv).has_value() == false);
    CHECK(crispy::to_integer<10>("bad"sv).has_value() == false);
}

TEST_CASE("utils.to_integer.2")
{
    CHECK(crispy::to_integer<2>("0"sv).value_or(-1) == 0);
    CHECK(crispy::to_integer<2>("10"sv).value_or(-1) == 0b10);
    CHECK(crispy::to_integer<2>("1100101"sv).value_or(-1) == 0b1100101);
}

TEST_CASE("utils.to_integer.10")
{
    CHECK(crispy::to_integer<10>("0"sv).value_or(-1) == 0);
    CHECK(crispy::to_integer<10>("9"sv).value_or(-1) == 9);
    CHECK(crispy::to_integer<10>("18"sv).value_or(-1) == 18);
    CHECK(crispy::to_integer<10>("321"sv).value_or(-1) == 321);
    CHECK(crispy::to_integer<10>("12345"sv).value_or(-1) == 12345);

    // defaulted base is base-10
    CHECK(crispy::to_integer("12345"sv).value_or(-1) == 12345);
}

TEST_CASE("utils.to_integer.16")
{
    // upper case hex digits
    CHECK(crispy::to_integer<16>("B"sv).value_or(-1) == 0x0B);
    CHECK(crispy::to_integer<16>("0B"sv).value_or(-1) == 0x0B);
    CHECK(crispy::to_integer<16>("B0"sv).value_or(-1) == 0xB0);
    CHECK(crispy::to_integer<16>("B0"sv).value_or(-1) == 0xB0);
    CHECK(crispy::to_integer<16>("ABCDEF"sv).value_or(-1) == 0xABCDEF);

    // lower case hex digits
    CHECK(crispy::to_integer<16>("b"sv).value_or(-1) == 0x0B);
    CHECK(crispy::to_integer<16>("0b"sv).value_or(-1) == 0x0B);
    CHECK(crispy::to_integer<16>("b0"sv).value_or(-1) == 0xB0);
    CHECK(crispy::to_integer<16>("b0"sv).value_or(-1) == 0xB0);
    CHECK(crispy::to_integer<16>("abcdef"sv).value_or(-1) == 0xABCDEF);

    CHECK(crispy::to_integer<16>("0"sv).value_or(-1) == 0x0);
    CHECK(crispy::to_integer<16>("9"sv).value_or(-1) == 0x9);
    CHECK(crispy::to_integer<16>("18"sv).value_or(-1) == 0x18);
    CHECK(crispy::to_integer<16>("321"sv).value_or(-1) == 0x321);
    CHECK(crispy::to_integer<16>("12345"sv).value_or(-1) == 0x12345);
}

TEST_CASE("fromHexString")
{
    CHECK(!crispy::fromHexString("abc"sv));
    CHECK(!crispy::fromHexString("GX"sv));

    CHECK(crispy::fromHexString(""sv).value().empty());
    CHECK(crispy::fromHexString("61"sv).value() == "a"sv);
    CHECK(crispy::fromHexString("4162"sv).value() == "Ab"sv);
}

struct variable_collector
{
    auto operator()(string_view name) const { return std::format("({})", name); }
};

TEST_CASE("replaceVariables")
{
    // clang-format off
    CHECK(crispy::replaceVariables("", variable_collector()).empty());
    CHECK("()"sv == crispy::replaceVariables("${}", variable_collector()));
    CHECK("(Hello)"sv == crispy::replaceVariables("${Hello}", variable_collector()));
    CHECK("(Hello) World"sv == crispy::replaceVariables("${Hello} World", variable_collector()));
    CHECK("Hello, (World)!"sv == crispy::replaceVariables("Hello, ${World}!", variable_collector()));
    CHECK("(one), (two), (three)"sv == crispy::replaceVariables("${one}, ${two}, ${three}", variable_collector()));
    // clang-format on
}

TEST_CASE("homeResolvedPath")
{
    CHECK(crispy::homeResolvedPath("", "/var/tmp").generic_string().empty());

    CHECK("/var/tmp/workspace" == crispy::homeResolvedPath("~/workspace", "/var/tmp").generic_string());
}

TEST_CASE("expandEnvironmentVariables")
{
    auto const envReplacer = [](string_view name) -> string {
        if (name == "HOME")
            return "/home/user";
        if (name == "SHELL")
            return "/bin/bash";
        return {};
    };

    // Known variables resolve correctly
    CHECK("/home/user/Pictures" == crispy::replaceVariables("${HOME}/Pictures", envReplacer));
    CHECK("/bin/bash" == crispy::replaceVariables("${SHELL}", envReplacer));

    // Multiple variables in one string
    CHECK("/home/user runs /bin/bash" == crispy::replaceVariables("${HOME} runs ${SHELL}", envReplacer));

    // Unknown variables expand to empty string
    CHECK("/Pictures" == crispy::replaceVariables("${UNDEFINED}/Pictures", envReplacer));

    // No markers at all — input passes through unchanged
    CHECK("/usr/local/bin" == crispy::replaceVariables("/usr/local/bin", envReplacer));

    // Malformed ${UNCLOSED at start of string passes through unchanged
    CHECK("${UNCLOSED" == crispy::replaceVariables("${UNCLOSED", envReplacer));
}

TEST_CASE("replaceVariables.and.homeResolvedPath.composition")
{
    auto const envReplacer = [](string_view name) -> string {
        if (name == "HOME")
            return "/home/user";
        if (name == "PICS")
            return "Pictures";
        return {};
    };

    auto const resolve = [&](string const& input) {
        return crispy::homeResolvedPath(crispy::replaceVariables(input, envReplacer), "/home/user");
    };

    // ${HOME}/path → env expansion → home resolution (no ~ involved)
    CHECK("/home/user/Pictures/bg.png" == resolve("${HOME}/Pictures/bg.png").generic_string());

    // ~/path → no env vars to expand → home resolution handles ~
    CHECK("/home/user/workspace" == resolve("~/workspace").generic_string());

    // Mixed: env var inside path with ~
    CHECK("/home/user/Pictures" == resolve("~/${PICS}").generic_string());
}

TEST_CASE("unescapeURL")
{
    CHECK(crispy::unescapeURL(""sv).empty());
    CHECK(crispy::unescapeURL("foo"sv) == "foo");
    CHECK(crispy::unescapeURL("foo%20bar"sv) == "foo bar");
    CHECK(crispy::unescapeURL("%20"sv) == " ");
    CHECK(crispy::unescapeURL("%2"sv) == "%2"); // incomplete hex
    CHECK(crispy::unescapeURL("%"sv) == "%");   // incomplete hex
    CHECK(crispy::unescapeURL("A%42C"sv) == "ABC");
    CHECK(crispy::unescapeURL("%gg"sv) == "%gg"); // invalid hex
}

TEST_CASE("for_each_key_value")
{
    auto result = std::map<std::string_view, std::string_view> {};
    auto collect = [&](std::string_view key, std::string_view value) {
        result[key] = value;
    };

    // empty input
    crispy::for_each_key_value({ .text = "", .entryDelimiter = ';', .assignmentDelimiter = '=' }, collect);
    CHECK(result.empty());

    // single element
    result.clear();
    crispy::for_each_key_value({ .text = "foo=bar", .entryDelimiter = ';', .assignmentDelimiter = '=' },
                               collect);
    CHECK(result.size() == 1);
    CHECK(result["foo"] == "bar");

    // two elements
    result.clear();
    crispy::for_each_key_value({ .text = "a=b;c=d", .entryDelimiter = ';', .assignmentDelimiter = '=' },
                               collect);
    CHECK(result.size() == 2);
    CHECK(result["a"] == "b");
    CHECK(result["c"] == "d");

    // empty element
    result.clear();
    crispy::for_each_key_value({ .text = "a=b;;c=d", .entryDelimiter = ';', .assignmentDelimiter = '=' },
                               collect);
    CHECK(result.size() == 2);
    CHECK(result["a"] == "b");
    CHECK(result["c"] == "d");

    // No delimiter
    result.clear();
    crispy::for_each_key_value({ .text = "key_only", .entryDelimiter = ';', .assignmentDelimiter = '=' },
                               collect);
    CHECK(result.size() == 1);
    CHECK(result["key_only"] == "");
}
