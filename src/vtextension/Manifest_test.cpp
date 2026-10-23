// SPDX-License-Identifier: Apache-2.0
#include <core/Sha256.hpp>
#include <core/Utils.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <vtextension/Manifest.hpp>

#ifdef VTEXTENSION_TEST_MODULE_DIR
    #include <vtextension/testing/TestModules.hpp>
#endif

using namespace std::string_view_literals;
using vtextension::ExtensionError;
using vtextension::parseManifest;

namespace
{

constexpr auto Example = "name = git\n"
                         "version = 1\n"
                         "command status = git -C {cwd} status --porcelain=v2 --branch\n"sv;

void appendBytes(std::vector<std::byte>& out, std::string_view text)
{
    std::ranges::transform(text, std::back_inserter(out), [](char c) { return static_cast<std::byte>(c); });
}

void appendLeb128(std::vector<std::byte>& out, uint32_t value)
{
    do
    {
        auto byte = static_cast<uint8_t>(value & 0x7FU);
        value >>= 7U;
        if (value != 0)
            byte |= 0x80U;
        out.push_back(static_cast<std::byte>(byte));
    } while (value != 0);
}

/// The 8-byte header every core module starts with.
[[nodiscard]] std::vector<std::byte> moduleHeader()
{
    auto out = std::vector<std::byte>();
    appendBytes(out, "\0asm\x01\0\0\0"sv);
    return out;
}

/// Appends a section: id, LEB128 size, payload.
void appendSection(std::vector<std::byte>& out, uint8_t id, std::span<std::byte const> payload)
{
    out.push_back(static_cast<std::byte>(id));
    appendLeb128(out, static_cast<uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
}

/// Appends a custom section named @p name carrying @p content.
void appendCustomSection(std::vector<std::byte>& out, std::string_view name, std::string_view content)
{
    auto payload = std::vector<std::byte>();
    appendLeb128(payload, static_cast<uint32_t>(name.size()));
    appendBytes(payload, name);
    appendBytes(payload, content);
    appendSection(out, 0, payload);
}

/// A module that carries @p manifest in the manifest section.
[[nodiscard]] std::vector<std::byte> moduleWith(std::string_view manifest)
{
    auto out = moduleHeader();
    appendCustomSection(out, vtextension::ManifestSectionName, manifest);
    return out;
}

} // namespace

TEST_CASE("Manifest.parse", "[vtextension]")
{
    auto const result = parseManifest(moduleWith(Example));
    REQUIRE(result.has_value());
    CHECK(result->name == "git");
    CHECK(result->version == 1);
    REQUIRE(result->commands.size() == 1);
    CHECK(result->commands[0].name == "status");
    CHECK(result->commands[0].argvTemplate
          == std::vector<std::string> { "git", "-C", "{cwd}", "status", "--porcelain=v2", "--branch" });
}

TEST_CASE("Manifest.digest", "[vtextension]")
{
    auto const result = parseManifest(moduleWith(Example));
    REQUIRE(result.has_value());
    auto const expected = core::toLowerHexString(core::Sha256::hash(Example));
    CHECK(result->digest == expected);
    CHECK(result->digest.size() == 64);

    auto const changed = parseManifest(moduleWith(std::string(Example) + "command more = true\n"));
    REQUIRE(changed.has_value());
    CHECK(changed->digest != result->digest);
}

TEST_CASE("Manifest.surroundedByOtherSections", "[vtextension]")
{
    auto module = moduleHeader();
    auto const code = std::array { std::byte { 0x01 }, std::byte { 0x02 }, std::byte { 0x03 } };
    appendSection(module, 1, code);
    appendCustomSection(module, "name", "unrelated");
    appendCustomSection(module, vtextension::ManifestSectionName, Example);
    appendSection(module, 10, code);

    auto const result = parseManifest(module);
    REQUIRE(result.has_value());
    CHECK(result->name == "git");
}

TEST_CASE("Manifest.layout", "[vtextension]")
{
    auto const result = parseManifest(
        moduleWith("# a comment\n\nname = x\r\nversion = 1\ncommand  a   =  run  one   {x}\n"sv));
    REQUIRE(result.has_value());
    CHECK(result->version == 1);
    REQUIRE(result->commands.size() == 1);
    CHECK(result->commands[0].argvTemplate == std::vector<std::string> { "run", "one", "{x}" });
}

TEST_CASE("Manifest.noCommands", "[vtextension]")
{
    auto const result = parseManifest(moduleWith("name = quiet\nversion = 1\n"sv));
    REQUIRE(result.has_value());
    CHECK(result->commands.empty());
}

TEST_CASE("Manifest.missing", "[vtextension]")
{
    SECTION("no sections at all")
    {
        auto const result = parseManifest(moduleHeader());
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ExtensionError::ManifestMissing);
    }
    SECTION("only other custom sections")
    {
        auto module = moduleHeader();
        appendCustomSection(module, "producers", "x");
        auto const result = parseManifest(module);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ExtensionError::ManifestMissing);
    }
    SECTION("a non-custom section is not the manifest")
    {
        auto module = moduleHeader();
        appendSection(module, 1, std::span<std::byte const> {});
        auto const result = parseManifest(module);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ExtensionError::ManifestMissing);
    }
}

TEST_CASE("Manifest.partialPlaceholder", "[vtextension]")
{
    auto const text = GENERATE(as<std::string_view> {},
                               "name = a\nversion = 1\ncommand c = tool --dir={cwd}\n",
                               "name = a\nversion = 1\ncommand c = tool x{cwd}\n",
                               "name = a\nversion = 1\ncommand c = tool {cwd}y\n",
                               "name = a\nversion = 1\ncommand c = tool {}\n",
                               "name = a\nversion = 1\ncommand c = tool {a{b}}\n",
                               "name = a\nversion = 1\ncommand c = tool }{\n");
    CAPTURE(text);
    auto const result = parseManifest(moduleWith(text));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ExtensionError::ManifestInvalid);
}

TEST_CASE("Manifest.placeholderAsProgram", "[vtextension]")
{
    auto const text = GENERATE(as<std::string_view> {},
                               "name = a\nversion = 1\ncommand run = {prog} {a}\n",
                               "name = a\nversion = 1\ncommand run = {prog}\n");
    CAPTURE(text);
    auto const result = parseManifest(moduleWith(text));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ExtensionError::ManifestInvalid);

    // A placeholder further along is fine.
    CHECK(parseManifest(moduleWith("name = a\nversion = 1\ncommand run = tool {a}\n")).has_value());
}

TEST_CASE("Manifest.placeholderNameCharacters", "[vtextension]")
{
    auto const element = GENERATE(as<std::string_view> {}, "{../x}", "{a/b}", "{a b}", "{a.b}", "{a$}");
    CAPTURE(element);
    CHECK_FALSE(vtextension::placeholderName(element).has_value());

    auto const text = "name = a\nversion = 1\ncommand c = tool " + std::string(element) + "\n";
    auto const result = parseManifest(moduleWith(text));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ExtensionError::ManifestInvalid);

    CHECK(vtextension::placeholderName("{Ab_9-z}") == "Ab_9-z");
}

TEST_CASE("Manifest.controlCharacters", "[vtextension]")
{
    auto const text = GENERATE(as<std::string_view> {},
                               "name = a\x1b[31m\nversion = 1\n",
                               "name = a\nversion = 1\ncommand c = tool \x1b]0;x\x07\n",
                               "name = a\nversion = 1\ncommand c = to\rol x\n",
                               "name = a\rb\nversion = 1\n",
                               "name = a\nversion = 1\r",
                               "name = a\nversion = 1\x7f\n",
                               "name = a\x08\nversion = 1\n",
                               "# \x1b\nname = a\nversion = 1\n");
    CAPTURE(text);
    auto const result = parseManifest(moduleWith(text));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ExtensionError::ManifestInvalid);
}

TEST_CASE("Manifest.invalidUtf8", "[vtextension]")
{
    auto const text = GENERATE(as<std::string_view> {},
                               "name = a\xff\nversion = 1\n",
                               "name = a\x80\nversion = 1\n",             // continuation byte without a lead
                               "name = a\xc3\nversion = 1\n",             // truncated sequence
                               "name = a\xc0\xaf\nversion = 1\n",         // overlong '/'
                               "name = a\xed\xa0\x80\nversion = 1\n",     // surrogate
                               "name = a\xf4\x90\x80\x80\nversion = 1\n", // above U+10FFFF
                               "name = a\xc2\x85\nversion = 1\n",         // C1 control U+0085
                               "name = a\xc2\x9f\nversion = 1\n");        // C1 control U+009F
    CAPTURE(text);
    auto const result = parseManifest(moduleWith(text));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ExtensionError::ManifestInvalid);
}

TEST_CASE("Manifest.validText", "[vtextension]")
{
    // Multi-byte UTF-8 and tabs are fine, and U+00A0 is not a control character.
    auto const result = parseManifest(
        moduleWith("name = caf\xc3\xa9 \xf0\x9f\x98\x80\nversion\t=\t1\r\ncommand c = tool\t\xc2\xa0\n"sv));
    REQUIRE(result.has_value());
    CHECK(result->name == "caf\xc3\xa9 \xf0\x9f\x98\x80");
    REQUIRE(result->commands.size() == 1);
    CHECK(result->commands[0].argvTemplate == std::vector<std::string> { "tool", "\xc2\xa0" });
}

TEST_CASE("Manifest.duplicateSection", "[vtextension]")
{
    // A linker concatenates same-named sections; the host reads exactly one, so two are refused.
    auto module = moduleHeader();
    appendCustomSection(module, vtextension::ManifestSectionName, Example);
    appendCustomSection(module, vtextension::ManifestSectionName, "name = evil\nversion = 1\n");
    auto const result = parseManifest(module);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ExtensionError::ManifestInvalid);
}

TEST_CASE("Manifest.invalidText", "[vtextension]")
{
    auto const text = GENERATE(as<std::string_view> {},
                               "",
                               "version = 1\n",
                               "name = a\n",
                               "name = a\nversion = one\n",
                               "name = a\nversion = 1\nname = b\n",
                               "name = a\nversion = 1\nversion = 2\n",
                               "name = \nversion = 1\n",
                               "name = a\nversion = 1\nwhat = ever\n",
                               "name = a\nversion = 1\nnonsense\n",
                               "name = a\nversion = 1\ncommand c =\n",
                               "name = a\nversion = 1\ncommand = x\n",
                               "name = a\nversion = 1\ncommand c = x\ncommand c = y\n",
                               "name = a\nversion = 99999999999\n",
                               "name = a\nversion = 0\n",
                               "name = a\nversion = 2\n");
    CAPTURE(text);
    auto const result = parseManifest(moduleWith(text));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ExtensionError::ManifestInvalid);
}

TEST_CASE("Manifest.malformedModule", "[vtextension]")
{
    SECTION("empty")
    {
        auto const result = parseManifest(std::span<std::byte const> {});
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ExtensionError::ManifestInvalid);
    }
    SECTION("bad magic")
    {
        auto module = moduleWith(Example);
        module[1] = std::byte { 'x' };
        auto const result = parseManifest(module);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ExtensionError::ManifestInvalid);
    }
    SECTION("unsupported version")
    {
        auto module = moduleWith(Example);
        module[4] = std::byte { 2 };
        auto const result = parseManifest(module);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ExtensionError::ManifestInvalid);
    }
    SECTION("section size beyond the end")
    {
        auto module = moduleHeader();
        module.push_back(std::byte { 1 });
        appendLeb128(module, 1000);
        module.push_back(std::byte { 0 });
        auto const result = parseManifest(module);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ExtensionError::ManifestInvalid);
    }
    SECTION("unterminated size")
    {
        auto module = moduleHeader();
        module.push_back(std::byte { 1 });
        module.push_back(std::byte { 0x80 });
        module.push_back(std::byte { 0x80 });
        auto const result = parseManifest(module);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ExtensionError::ManifestInvalid);
    }
    SECTION("size longer than five bytes")
    {
        auto module = moduleHeader();
        module.push_back(std::byte { 1 });
        module.insert(module.end(), 6, std::byte { 0x80 });
        module.push_back(std::byte { 0 });
        auto const result = parseManifest(module);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ExtensionError::ManifestInvalid);
    }
    SECTION("size above 32 bits")
    {
        auto module = moduleHeader();
        module.push_back(std::byte { 1 });
        for (auto const byte: std::array<uint8_t, 5> { 0xFF, 0xFF, 0xFF, 0xFF, 0x1F })
            module.push_back(static_cast<std::byte>(byte));
        module.push_back(std::byte { 0 });
        auto const result = parseManifest(module);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ExtensionError::ManifestInvalid);
    }
    SECTION("name longer than its section")
    {
        auto module = moduleHeader();
        auto payload = std::vector<std::byte>();
        appendLeb128(payload, 200);
        appendBytes(payload, "abc"sv);
        appendSection(module, 0, payload);
        auto const result = parseManifest(module);
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == ExtensionError::ManifestInvalid);
    }
    SECTION("every truncation of a valid module is an error, never a crash")
    {
        auto const module = moduleWith(Example);
        for (auto const length: std::views::iota(std::size_t { 0 }, module.size()))
        {
            auto const result = parseManifest(std::span { module }.first(length));
            CAPTURE(length);
            REQUIRE_FALSE(result.has_value());
        }
    }
}

TEST_CASE("Manifest.placeholderName", "[vtextension]")
{
    CHECK(vtextension::placeholderName("{cwd}") == "cwd");
    CHECK_FALSE(vtextension::placeholderName("{}").has_value());
    CHECK_FALSE(vtextension::placeholderName("cwd").has_value());
    CHECK_FALSE(vtextension::placeholderName("--dir={cwd}").has_value());
    CHECK_FALSE(vtextension::placeholderName("{a}b").has_value());
    CHECK_FALSE(vtextension::placeholderName("{a{b}}").has_value());
    CHECK_FALSE(vtextension::placeholderName("").has_value());
}

#ifdef VTEXTENSION_TEST_MODULE_DIR
TEST_CASE("Manifest.fromGuest", "[vtextension][guest]")
{
    // The echo guest embeds its manifest with the Rust helper crate's declare_manifest!.
    auto const bytes = vtextension::testing::readTestModule("echo.wasm");
    auto const result = parseManifest(bytes);
    REQUIRE(result.has_value());
    CHECK(result->name == "echo");
    CHECK(result->version == 1);
    REQUIRE(result->commands.size() == 2);
    CHECK(result->commands[0].name == "greet");
    CHECK(result->commands[0].argvTemplate == std::vector<std::string> { "echo", "hello", "{name}" });
    CHECK(result->commands[1].name == "plain");
    CHECK(result->digest.size() == 64);

    // A module without the section reports it missing.
    auto const hostile = vtextension::testing::readTestModule("hostile.wasm");
    auto const missing = parseManifest(hostile);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error() == ExtensionError::ManifestMissing);
}
#else
TEST_CASE("Manifest.fromGuest", "[vtextension][guest]")
{
    SKIP("the Rust test guests are not built (CONTOUR_EXTENSIONS_BUNDLED=OFF)");
}
#endif
