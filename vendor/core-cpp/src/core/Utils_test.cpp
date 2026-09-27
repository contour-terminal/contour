// SPDX-License-Identifier: Apache-2.0
#include <core/Utils.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <fstream>
#include <map>
#include <vector>

using std::string;
using std::string_view;
using namespace std::string_literals;
using namespace std::string_view_literals;

TEST_CASE("utils.split.0")
{
    auto result = core::splitKeyValuePairs("", ':');
    CHECK(result.empty());
}

TEST_CASE("utils.split.1")
{
    auto result = core::splitKeyValuePairs("foo=bar", ':');
    CHECK(result.size() == 1);
    CHECK(result["foo"] == "bar");

    auto result2 = core::splitKeyValuePairs("foo=bar::", ':');
    CHECK(result2.size() == 1);
    CHECK(result2["foo"] == "bar");

    auto result3 = core::splitKeyValuePairs("::foo=bar", ':');
    CHECK(result3.size() == 1);
    CHECK(result3["foo"] == "bar");
}

TEST_CASE("utils.split.2")
{
    auto result = core::splitKeyValuePairs("foo=bar:fnord=tar", ':');
    CHECK(result.size() == 2);
    CHECK(result["foo"] == "bar");
    CHECK(result["fnord"] == "tar");

    auto result2 = core::splitKeyValuePairs("foo=bar::fnord=tar", ':');
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
        std::is_same_v<uint8_t, std::remove_reference_t<decltype(*core::toInteger<10, uint8_t>(""sv))>>);

    static_assert(std::is_same_v<int, std::remove_reference_t<decltype(*core::toInteger<10, int>(""sv))>>);

    static_assert(
        std::is_same_v<unsigned, std::remove_reference_t<decltype(*core::toInteger<10, unsigned>(""sv))>>);

    static_assert(
        std::is_same_v<uint64_t, std::remove_reference_t<decltype(*core::toInteger<10, uint64_t>(""sv))>>);
}

TEST_CASE("utils.toInteger.bad")
{
    CHECK(core::toInteger<10>(""sv).has_value() == false);
    CHECK(core::toInteger<10>("bad"sv).has_value() == false);
}

TEST_CASE("utils.toInteger.2")
{
    CHECK(core::toInteger<2>("0"sv).value_or(-1) == 0);
    CHECK(core::toInteger<2>("10"sv).value_or(-1) == 0b10);
    CHECK(core::toInteger<2>("1100101"sv).value_or(-1) == 0b1100101);
}

TEST_CASE("utils.toInteger.10")
{
    CHECK(core::toInteger<10>("0"sv).value_or(-1) == 0);
    CHECK(core::toInteger<10>("9"sv).value_or(-1) == 9);
    CHECK(core::toInteger<10>("18"sv).value_or(-1) == 18);
    CHECK(core::toInteger<10>("321"sv).value_or(-1) == 321);
    CHECK(core::toInteger<10>("12345"sv).value_or(-1) == 12345);

    // defaulted base is base-10
    CHECK(core::toInteger("12345"sv).value_or(-1) == 12345);
}

TEST_CASE("utils.toInteger.16")
{
    // upper case hex digits
    CHECK(core::toInteger<16>("B"sv).value_or(-1) == 0x0B);
    CHECK(core::toInteger<16>("0B"sv).value_or(-1) == 0x0B);
    CHECK(core::toInteger<16>("B0"sv).value_or(-1) == 0xB0);
    CHECK(core::toInteger<16>("B0"sv).value_or(-1) == 0xB0);
    CHECK(core::toInteger<16>("ABCDEF"sv).value_or(-1) == 0xABCDEF);

    // lower case hex digits
    CHECK(core::toInteger<16>("b"sv).value_or(-1) == 0x0B);
    CHECK(core::toInteger<16>("0b"sv).value_or(-1) == 0x0B);
    CHECK(core::toInteger<16>("b0"sv).value_or(-1) == 0xB0);
    CHECK(core::toInteger<16>("b0"sv).value_or(-1) == 0xB0);
    CHECK(core::toInteger<16>("abcdef"sv).value_or(-1) == 0xABCDEF);

    CHECK(core::toInteger<16>("0"sv).value_or(-1) == 0x0);
    CHECK(core::toInteger<16>("9"sv).value_or(-1) == 0x9);
    CHECK(core::toInteger<16>("18"sv).value_or(-1) == 0x18);
    CHECK(core::toInteger<16>("321"sv).value_or(-1) == 0x321);
    CHECK(core::toInteger<16>("12345"sv).value_or(-1) == 0x12345);
}

TEST_CASE("utils.toInteger.8")
{
    CHECK(core::toInteger<8>("0"sv).value_or(-1) == 0);
    CHECK(core::toInteger<8>("7"sv).value_or(-1) == 07);
    CHECK(core::toInteger<8>("644"sv).value_or(-1) == 0644);

    // 8 and 9 are not octal digits.
    CHECK(!core::toInteger<8>("8"sv).has_value());
    CHECK(!core::toInteger<8>("19"sv).has_value());
}

// Most of what reaches toInteger() is escape-sequence text written by the connected program, which
// bounds neither its length nor its magnitude. Accumulating it unchecked overflowed: for the signed
// instantiations that is undefined behaviour, and for the unsigned ones it silently wrapped to a
// number the sender never wrote.
TEST_CASE("utils.toInteger.overflow")
{
    SECTION("a value too large for the result type is rejected, not wrapped")
    {
        CHECK(!core::toInteger<10, uint8_t>("256"sv).has_value());
        CHECK(!core::toInteger<10, uint8_t>("999"sv).has_value());
        CHECK(!core::toInteger<10, int>("2147483648"sv).has_value());
        CHECK(!core::toInteger<10, unsigned>("4294967296"sv).has_value());
        CHECK(!core::toInteger<16, uint32_t>("100000000"sv).has_value());
    }

    SECTION("the largest representable value still parses")
    {
        CHECK(core::toInteger<10, uint8_t>("255"sv) == 255);
        CHECK(core::toInteger<10, int>("2147483647"sv) == 2147483647);
        CHECK(core::toInteger<10, unsigned>("4294967295"sv) == 4294967295U);
        CHECK(core::toInteger<16, uint32_t>("ffffffff"sv) == 0xFFFFFFFFU);
    }

    SECTION("an arbitrarily long digit run terminates without undefined behaviour")
    {
        // The OSC 133;D payload that made the shell-integration exit code overflow a signed int.
        CHECK(!core::toInteger<10, int>("99999999999"sv).has_value());
        CHECK(!core::toInteger<10, uint64_t>(std::string(64, '9')).has_value());
    }

    SECTION("leading zeroes do not count towards the range")
    {
        CHECK(core::toInteger<10, uint8_t>("00000000000000255"sv) == 255);
    }
}

TEST_CASE("fromHexString")
{
    CHECK(!core::fromHexString("abc"sv));
    CHECK(!core::fromHexString("GX"sv));

    CHECK(core::fromHexString(""sv).value().empty());
    CHECK(core::fromHexString("61"sv).value() == "a"sv);
    CHECK(core::fromHexString("4162"sv).value() == "Ab"sv);
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
    CHECK(core::replaceVariables("", VariableCollector()).empty());
    CHECK("()"sv == core::replaceVariables("${}", VariableCollector()));
    CHECK("(Hello)"sv == core::replaceVariables("${Hello}", VariableCollector()));
    CHECK("(Hello) World"sv == core::replaceVariables("${Hello} World", VariableCollector()));
    CHECK("Hello, (World)!"sv == core::replaceVariables("Hello, ${World}!", VariableCollector()));
    CHECK("(one), (two), (three)"sv == core::replaceVariables("${one}, ${two}, ${three}", VariableCollector()));

    // "$${" escapes expansion to a literal "${...}" (SaveLayout round-trip of literal ${...} text).
    CHECK("${Hello}"sv == core::replaceVariables("$${Hello}", VariableCollector()));
    CHECK("${a} (b)"sv == core::replaceVariables("$${a} ${b}", VariableCollector()));
    CHECK("s/${VERSION}/1.0/"sv == core::replaceVariables("s/$${VERSION}/1.0/", VariableCollector()));
    CHECK("a $ b"sv == core::replaceVariables("a $ b", VariableCollector())); // lone '$' untouched
    // clang-format on
}

TEST_CASE("homeResolvedPath")
{
    CHECK(core::homeResolvedPath("", "/var/tmp").generic_string().empty());

    CHECK("/var/tmp/workspace" == core::homeResolvedPath("~/workspace", "/var/tmp").generic_string());
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
    CHECK("/home/user/Pictures" == core::replaceVariables("${HOME}/Pictures", envReplacer));
    CHECK("/bin/bash" == core::replaceVariables("${SHELL}", envReplacer));

    // Multiple variables in one string
    CHECK("/home/user runs /bin/bash" == core::replaceVariables("${HOME} runs ${SHELL}", envReplacer));

    // Unknown variables expand to empty string
    CHECK("/Pictures" == core::replaceVariables("${UNDEFINED}/Pictures", envReplacer));

    // No markers at all — input passes through unchanged
    CHECK("/usr/local/bin" == core::replaceVariables("/usr/local/bin", envReplacer));

    // Malformed ${UNCLOSED at start of string passes through unchanged
    CHECK("${UNCLOSED" == core::replaceVariables("${UNCLOSED", envReplacer));
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
        return core::homeResolvedPath(core::replaceVariables(input, envReplacer), "/home/user");
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
    CHECK(core::unescapeURL(""sv).empty());
    CHECK(core::unescapeURL("foo"sv) == "foo");
    CHECK(core::unescapeURL("foo%20bar"sv) == "foo bar");
    CHECK(core::unescapeURL("%20"sv) == " ");
    CHECK(core::unescapeURL("%2"sv) == "%2"); // incomplete hex
    CHECK(core::unescapeURL("%"sv) == "%");   // incomplete hex
    CHECK(core::unescapeURL("A%42C"sv) == "ABC");
    CHECK(core::unescapeURL("%gg"sv) == "%gg"); // invalid hex
}

TEST_CASE("forEachKeyValue")
{
    auto result = std::map<std::string_view, std::string_view> {};
    auto collect = [&](std::string_view key, std::string_view value) {
        result[key] = value;
    };

    // empty input
    core::forEachKeyValue({ .text = "", .entryDelimiter = ';', .assignmentDelimiter = '=' }, collect);
    CHECK(result.empty());

    // single element
    result.clear();
    core::forEachKeyValue({ .text = "foo=bar", .entryDelimiter = ';', .assignmentDelimiter = '=' }, collect);
    CHECK(result.size() == 1);
    CHECK(result["foo"] == "bar");

    // two elements
    result.clear();
    core::forEachKeyValue({ .text = "a=b;c=d", .entryDelimiter = ';', .assignmentDelimiter = '=' }, collect);
    CHECK(result.size() == 2);
    CHECK(result["a"] == "b");
    CHECK(result["c"] == "d");

    // empty element
    result.clear();
    core::forEachKeyValue({ .text = "a=b;;c=d", .entryDelimiter = ';', .assignmentDelimiter = '=' }, collect);
    CHECK(result.size() == 2);
    CHECK(result["a"] == "b");
    CHECK(result["c"] == "d");

    // No delimiter
    result.clear();
    core::forEachKeyValue({ .text = "key_only", .entryDelimiter = ';', .assignmentDelimiter = '=' }, collect);
    CHECK(result.size() == 1);
    CHECK(result["key_only"].empty());
}

TEST_CASE("utils.nextPowerOfTwo")
{
    STATIC_CHECK(core::nextPowerOfTwo(1U) == 1U);
    STATIC_CHECK(core::nextPowerOfTwo(5U) == 8U);
    STATIC_CHECK(core::nextPowerOfTwo(8U) == 8U);

    // Set bits further apart than the smear reaches: every bit below the highest one must be
    // filled, whatever the width of the type.
    STATIC_CHECK(core::nextPowerOfTwo(std::uint32_t { 257 }) == 512U);
    STATIC_CHECK(core::nextPowerOfTwo(std::uint32_t { 0x1'0001 }) == 0x2'0000U);
    STATIC_CHECK(core::nextPowerOfTwo(std::uint32_t { 0x4000'0001 }) == 0x8000'0000U);
    STATIC_CHECK(core::nextPowerOfTwo(std::uint64_t { 0x1'0000'0001 }) == 0x2'0000'0000U);
    STATIC_CHECK(core::nextPowerOfTwo(std::uint64_t { 0x4000'0000'0000'0001 }) == 0x8000'0000'0000'0000U);
}

TEST_CASE("utils.trim")
{
    // constexpr, so the whole table is settled at compile time -- the cases a key-file parser
    // relies on, which must read `shared = ipc ;` as readily as `shared=ipc;`.
    STATIC_CHECK(core::trim("  padded  "sv) == "padded"sv);
    STATIC_CHECK(core::trimLeft("  padded  "sv) == "padded  "sv);
    STATIC_CHECK(core::trimRight("  padded  "sv) == "  padded"sv);

    // Every character of the set, at both ends, and interior ones left alone.
    STATIC_CHECK(core::trim(" \t\r\nx y\t \r\n"sv) == "x y"sv);

    // Nothing to trim, nothing but whitespace, and nothing at all.
    STATIC_CHECK(core::trim("bare"sv) == "bare"sv);
    STATIC_CHECK(core::trim(" \t\r\n"sv).empty());
    STATIC_CHECK(core::trim(""sv).empty());
    STATIC_CHECK(core::trimLeft(" \t"sv).empty());
    STATIC_CHECK(core::trimRight(" \t"sv).empty());
}

// A string_view carries its length and promises nothing beyond it. The last segment was rebuilt
// with the length-less std::string_view(char const*) constructor, which calls strlen(): it read
// past the view, and returned whatever followed as part of the value.
TEST_CASE("utils.splitKeyValuePairs.bounded")
{
    SECTION("the last segment stops at the end of the view, not at the next NUL")
    {
        auto const backing = "foo=bar:trailing=junk"s;
        auto const view = string_view { backing }.substr(0, 7); // "foo=bar"
        auto const result = core::splitKeyValuePairs(view, ':');
        CHECK(result.size() == 1);
        CHECK(result.at("foo") == "bar"sv);
    }

    SECTION("a view whose last byte is the last byte of its allocation is not read past")
    {
        // No NUL anywhere in the buffer: strlen() would run off the end of the heap block,
        // which is what AddressSanitizer reports.
        auto const backing = std::vector<char> { 'f', 'o', 'o', '=', 'b', 'a', 'r' };
        auto const result = core::splitKeyValuePairs(string_view { backing.data(), backing.size() }, ':');
        CHECK(result.size() == 1);
        CHECK(result.at("foo") == "bar"sv);
    }
}

// tolower()/toupper() are undefined for a char whose value is negative, which every continuation
// byte of a UTF-8 sequence is. cli::about::registerProjects() sorts project titles through these.
TEST_CASE("utils.toLower/toUpper.non-ascii")
{
    auto const utf8 = "Grüße, WELT"sv; // 'ü' and 'ß' are two bytes each, both with the high bit set

    SECTION("bytes outside ASCII pass through unchanged")
    {
        CHECK(core::toLower(utf8) == "grüße, welt"sv);
        CHECK(core::toUpper(utf8) == "GRüßE, WELT"sv);
    }

    SECTION("every byte value is accepted")
    {
        auto every = string {};
        for (auto const i: std::views::iota(0, 256))
            every.push_back(static_cast<char>(i));
        CHECK(core::toLower(string_view { every }).size() == every.size());
        CHECK(core::toUpper(string_view { every }).size() == every.size());
    }
}

TEST_CASE("utils.readFileAsString")
{
    auto const tmp = core::testing::ScopedTempDir { "core-cpp-utils" };

    auto const write = [](std::filesystem::path const& path, string_view bytes) {
        auto out = std::ofstream { path, std::ios::binary };
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    };

    SECTION("the bytes on disk come back, and nothing else")
    {
        // CRLF on purpose: opened in text mode, Windows delivers fewer bytes than file_size()
        // reported, and the shortfall stayed behind as trailing NULs.
        auto const path = tmp / "crlf.txt";
        auto const content = "line one\r\nline two\r\n"sv;
        write(path, content);
        CHECK(core::readFileAsString(path) == content);
    }

    SECTION("an empty file reads as an empty string")
    {
        auto const path = tmp / "empty.txt";
        write(path, ""sv);
        CHECK(core::readFileAsString(path).empty());
    }

    SECTION("a file that is not there reads as empty, as the contract says")
    {
        // file_size() throws for a missing path; contour reads a CA certificate through this,
        // so a missing file is the ordinary failure and must be a value, not an exception.
        CHECK(core::readFileAsString(tmp / "no-such-file.txt").empty());
        CHECK_NOTHROW(core::readFileAsString(tmp / "nor" / "this" / "one.txt"));
    }

    SECTION("a path that is not representable in the native narrow encoding still opens")
    {
        auto const path = tmp.path() / std::filesystem::path(u8"grüße-日本語.txt");
        auto const content = "payload"sv;
        write(path, content);
        CHECK(core::readFileAsString(path) == content);
    }
}

TEST_CASE("utils.eachElement")
{
    SECTION("an unsigned type is walked from its minimum to its maximum, inclusive")
    {
        // end() was numeric_limits<T>::max() + 1 computed in int and cast back, which for an
        // 8-bit type is 0 -- equal to begin(), so the range was empty.
        auto count = 0;
        auto last = 0;
        for (auto const value: core::eachElement<uint8_t>())
        {
            ++count;
            last = value;
        }
        CHECK(count == 256);
        CHECK(last == 255);
    }

    SECTION("a signed type is walked from its minimum to its maximum, inclusive")
    {
        auto count = 0;
        auto first = int8_t {};
        auto last = int8_t {};
        for (auto const value: core::eachElement<int8_t>())
        {
            if (count == 0)
                first = value;
            ++count;
            last = value;
        }
        CHECK(count == 256);
        CHECK(first == std::numeric_limits<int8_t>::min());
        CHECK(last == std::numeric_limits<int8_t>::max());
    }

    SECTION("a type as wide as int is not walked into an overflow")
    {
        // Not iterated to exhaustion -- four billion steps -- but end() must not have wrapped
        // onto begin(), which would make the range empty and the loop body unreachable.
        auto count = uint32_t { 0 };
        for (auto const value: core::eachElement<uint32_t>())
        {
            CHECK(value == count);
            if (++count == 4U)
                break;
        }
        CHECK(count == 4U);
    }
}

TEST_CASE("utils.threadName")
{
    // The Windows path resized by the conversion length minus one, which underflowed when the
    // conversion failed and threw before the buffer was freed.
    CHECK_NOTHROW(core::threadName());
}

// The separator was a deduced template parameter, so its default could never be taken and the
// one-argument call did not compile.
TEST_CASE("utils.joinHumanReadableQuoted")
{
    auto const words = std::vector<string_view> { "one"sv, "two"sv };

    CHECK(core::joinHumanReadableQuoted(words) == "\"one\", \"two\"");
    CHECK(core::joinHumanReadableQuoted(words, " | ") == "\"one\" | \"two\"");
    CHECK(core::joinHumanReadableQuoted(std::vector<string_view> {}).empty());

    // Each element is escaped, which is the whole point of the quoted form: the tab comes out
    // as the two characters a backslash and a 't', not as a tab.
    CHECK(core::joinHumanReadableQuoted(std::vector<string_view> { "a\tb"sv }) == R"("a\tb")");
}
