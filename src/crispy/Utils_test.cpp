// SPDX-License-Identifier: Apache-2.0
#include <crispy/Utils.hpp>

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

TEST_CASE("utils.toInteger.integer_type")
{
    static_assert(
        std::is_same_v<uint8_t, std::remove_reference_t<decltype(*crispy::toInteger<10, uint8_t>(""sv))>>);

    static_assert(std::is_same_v<int, std::remove_reference_t<decltype(*crispy::toInteger<10, int>(""sv))>>);

    static_assert(
        std::is_same_v<unsigned, std::remove_reference_t<decltype(*crispy::toInteger<10, unsigned>(""sv))>>);

    static_assert(
        std::is_same_v<uint64_t, std::remove_reference_t<decltype(*crispy::toInteger<10, uint64_t>(""sv))>>);
}

TEST_CASE("utils.toInteger.bad")
{
    CHECK(crispy::toInteger<10>(""sv).has_value() == false);
    CHECK(crispy::toInteger<10>("bad"sv).has_value() == false);
}

TEST_CASE("utils.toInteger.2")
{
    CHECK(crispy::toInteger<2>("0"sv).value_or(-1) == 0);
    CHECK(crispy::toInteger<2>("10"sv).value_or(-1) == 0b10);
    CHECK(crispy::toInteger<2>("1100101"sv).value_or(-1) == 0b1100101);
}

TEST_CASE("utils.toInteger.10")
{
    CHECK(crispy::toInteger<10>("0"sv).value_or(-1) == 0);
    CHECK(crispy::toInteger<10>("9"sv).value_or(-1) == 9);
    CHECK(crispy::toInteger<10>("18"sv).value_or(-1) == 18);
    CHECK(crispy::toInteger<10>("321"sv).value_or(-1) == 321);
    CHECK(crispy::toInteger<10>("12345"sv).value_or(-1) == 12345);

    // defaulted base is base-10
    CHECK(crispy::toInteger("12345"sv).value_or(-1) == 12345);
}

TEST_CASE("utils.toInteger.16")
{
    // upper case hex digits
    CHECK(crispy::toInteger<16>("B"sv).value_or(-1) == 0x0B);
    CHECK(crispy::toInteger<16>("0B"sv).value_or(-1) == 0x0B);
    CHECK(crispy::toInteger<16>("B0"sv).value_or(-1) == 0xB0);
    CHECK(crispy::toInteger<16>("B0"sv).value_or(-1) == 0xB0);
    CHECK(crispy::toInteger<16>("ABCDEF"sv).value_or(-1) == 0xABCDEF);

    // lower case hex digits
    CHECK(crispy::toInteger<16>("b"sv).value_or(-1) == 0x0B);
    CHECK(crispy::toInteger<16>("0b"sv).value_or(-1) == 0x0B);
    CHECK(crispy::toInteger<16>("b0"sv).value_or(-1) == 0xB0);
    CHECK(crispy::toInteger<16>("b0"sv).value_or(-1) == 0xB0);
    CHECK(crispy::toInteger<16>("abcdef"sv).value_or(-1) == 0xABCDEF);

    CHECK(crispy::toInteger<16>("0"sv).value_or(-1) == 0x0);
    CHECK(crispy::toInteger<16>("9"sv).value_or(-1) == 0x9);
    CHECK(crispy::toInteger<16>("18"sv).value_or(-1) == 0x18);
    CHECK(crispy::toInteger<16>("321"sv).value_or(-1) == 0x321);
    CHECK(crispy::toInteger<16>("12345"sv).value_or(-1) == 0x12345);
}

TEST_CASE("utils.toInteger.8")
{
    CHECK(crispy::toInteger<8>("0"sv).value_or(-1) == 0);
    CHECK(crispy::toInteger<8>("7"sv).value_or(-1) == 07);
    CHECK(crispy::toInteger<8>("644"sv).value_or(-1) == 0644);

    // 8 and 9 are not octal digits.
    CHECK(!crispy::toInteger<8>("8"sv).has_value());
    CHECK(!crispy::toInteger<8>("19"sv).has_value());
}

// Most of what reaches toInteger() is escape-sequence text written by the connected program, which
// bounds neither its length nor its magnitude. Accumulating it unchecked overflowed: for the signed
// instantiations that is undefined behaviour, and for the unsigned ones it silently wrapped to a
// number the sender never wrote.
TEST_CASE("utils.toInteger.overflow")
{
    SECTION("a value too large for the result type is rejected, not wrapped")
    {
        CHECK(!crispy::toInteger<10, uint8_t>("256"sv).has_value());
        CHECK(!crispy::toInteger<10, uint8_t>("999"sv).has_value());
        CHECK(!crispy::toInteger<10, int>("2147483648"sv).has_value());
        CHECK(!crispy::toInteger<10, unsigned>("4294967296"sv).has_value());
        CHECK(!crispy::toInteger<16, uint32_t>("100000000"sv).has_value());
    }

    SECTION("the largest representable value still parses")
    {
        CHECK(crispy::toInteger<10, uint8_t>("255"sv) == 255);
        CHECK(crispy::toInteger<10, int>("2147483647"sv) == 2147483647);
        CHECK(crispy::toInteger<10, unsigned>("4294967295"sv) == 4294967295U);
        CHECK(crispy::toInteger<16, uint32_t>("ffffffff"sv) == 0xFFFFFFFFU);
    }

    SECTION("an arbitrarily long digit run terminates without undefined behaviour")
    {
        // The OSC 133;D payload that made the shell-integration exit code overflow a signed int.
        CHECK(!crispy::toInteger<10, int>("99999999999"sv).has_value());
        CHECK(!crispy::toInteger<10, uint64_t>(std::string(64, '9')).has_value());
    }

    SECTION("leading zeroes do not count towards the range")
    {
        CHECK(crispy::toInteger<10, uint8_t>("00000000000000255"sv) == 255);
    }
}

TEST_CASE("fromHexString")
{
    CHECK(!crispy::fromHexString("abc"sv));
    CHECK(!crispy::fromHexString("GX"sv));

    CHECK(crispy::fromHexString(""sv).value().empty());
    CHECK(crispy::fromHexString("61"sv).value() == "a"sv);
    CHECK(crispy::fromHexString("4162"sv).value() == "Ab"sv);
}

namespace
{
struct VariableCollector
{
    auto operator()(string_view name) const { return std::format("({})", name); }
};
} // namespace

TEST_CASE("replaceVariables")
{
    // clang-format off
    CHECK(crispy::replaceVariables("", VariableCollector()).empty());
    CHECK("()"sv == crispy::replaceVariables("${}", VariableCollector()));
    CHECK("(Hello)"sv == crispy::replaceVariables("${Hello}", VariableCollector()));
    CHECK("(Hello) World"sv == crispy::replaceVariables("${Hello} World", VariableCollector()));
    CHECK("Hello, (World)!"sv == crispy::replaceVariables("Hello, ${World}!", VariableCollector()));
    CHECK("(one), (two), (three)"sv == crispy::replaceVariables("${one}, ${two}, ${three}", VariableCollector()));

    // "$${" escapes expansion to a literal "${...}" (SaveLayout round-trip of literal ${...} text).
    CHECK("${Hello}"sv == crispy::replaceVariables("$${Hello}", VariableCollector()));
    CHECK("${a} (b)"sv == crispy::replaceVariables("$${a} ${b}", VariableCollector()));
    CHECK("s/${VERSION}/1.0/"sv == crispy::replaceVariables("s/$${VERSION}/1.0/", VariableCollector()));
    CHECK("a $ b"sv == crispy::replaceVariables("a $ b", VariableCollector())); // lone '$' untouched
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

TEST_CASE("forEachKeyValue")
{
    auto result = std::map<std::string_view, std::string_view> {};
    auto collect = [&](std::string_view key, std::string_view value) {
        result[key] = value;
    };

    // empty input
    crispy::forEachKeyValue({ .text = "", .entryDelimiter = ';', .assignmentDelimiter = '=' }, collect);
    CHECK(result.empty());

    // single element
    result.clear();
    crispy::forEachKeyValue({ .text = "foo=bar", .entryDelimiter = ';', .assignmentDelimiter = '=' },
                            collect);
    CHECK(result.size() == 1);
    CHECK(result["foo"] == "bar");

    // two elements
    result.clear();
    crispy::forEachKeyValue({ .text = "a=b;c=d", .entryDelimiter = ';', .assignmentDelimiter = '=' },
                            collect);
    CHECK(result.size() == 2);
    CHECK(result["a"] == "b");
    CHECK(result["c"] == "d");

    // empty element
    result.clear();
    crispy::forEachKeyValue({ .text = "a=b;;c=d", .entryDelimiter = ';', .assignmentDelimiter = '=' },
                            collect);
    CHECK(result.size() == 2);
    CHECK(result["a"] == "b");
    CHECK(result["c"] == "d");

    // No delimiter
    result.clear();
    crispy::forEachKeyValue({ .text = "key_only", .entryDelimiter = ';', .assignmentDelimiter = '=' },
                            collect);
    CHECK(result.size() == 1);
    CHECK(result["key_only"].empty());
}
