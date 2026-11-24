// SPDX-License-Identifier: Apache-2.0
#include <core/Sha256.hpp>
#include <core/Utils.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <vtextension/Manifest.hpp>

#ifdef VTEXTENSION_TEST_MODULE_DIR
    #include <vtextension/testing/TestModules.hpp>
#endif

using namespace std::string_view_literals;
using vtextension::ExtensionError;
using vtextension::ManifestFault;
using vtextension::parseManifest;
using vtextension::parseManifestText;

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
    auto const expected = core::toLowerHexString(core::Sha256::hash(moduleWith(Example)));
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

TEST_CASE("manifest control lines", "[vtextension][manifest][control]")
{
    using vtcontrol::Permission;
    auto const result =
        parseManifest(moduleWith("name = agent\nversion = 1\ncontrol read\ncontrol  input \n"sv));
    REQUIRE(result.has_value());
    CHECK(result->control == std::vector { Permission::Read, Permission::Input });
    CHECK(result->commands.empty());

    auto const none = parseManifest(moduleWith(Example));
    REQUIRE(none.has_value());
    CHECK(none->control.empty());

    auto const text = GENERATE(as<std::string_view> {},
                               "name = a\nversion = 1\ncontrol fly\n",
                               "name = a\nversion = 1\ncontrol read\ncontrol read\n",
                               "name = a\nversion = 1\ncontrol\n",
                               "name = a\nversion = 1\ncontrol read write\n",
                               "name = a\nversion = 1\ncontrol = read\n",
                               "name = a\nversion = 1\ncontrol Read\n");
    CAPTURE(text);
    auto const invalid = parseManifest(moduleWith(text));
    REQUIRE_FALSE(invalid.has_value());
    CHECK(invalid.error() == ExtensionError::ManifestInvalid);
}

TEST_CASE("manifest control lines change the digest", "[vtextension][manifest][control]")
{
    // A grant is bound to the digest, so declaring a new permission asks again.
    auto const before = parseManifest(moduleWith(Example));
    auto const after = parseManifest(moduleWith(std::string(Example) + "control write\n"));
    REQUIRE(before.has_value());
    REQUIRE(after.has_value());
    CHECK(before->digest != after->digest);
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
                               "name = a\nversion = 3\n");
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

// {{{ version 2

namespace
{

/// The manifest-v2 conformance corpus the orchestrator extension ships and tests against too.
[[nodiscard]] std::filesystem::path corpusDirectory()
{
    return { VTEXTENSION_MANIFEST_CORPUS_DIR };
}

[[nodiscard]] std::string readCorpusFile(std::filesystem::path const& path)
{
    auto file = std::ifstream(path, std::ios::binary);
    REQUIRE(file);
    auto text = std::ostringstream {};
    text << file.rdbuf();
    return std::move(text).str();
}

/// The names of the `.manifest` files in one corpus directory, without their extension.
[[nodiscard]] std::set<std::string> corpusFiles(std::string_view directory)
{
    auto names = std::set<std::string> {};
    for (auto const& entry: std::filesystem::directory_iterator(corpusDirectory() / directory))
        if (entry.path().extension() == ".manifest")
            names.insert(entry.path().stem().string());
    return names;
}

struct InvalidRow
{
    std::string_view file;
    ManifestFault fault;
};

constexpr auto InvalidRows = std::array {
    InvalidRow { .file = "brace-inside-word", .fault = ManifestFault::BraceInsideWord },
    InvalidRow { .file = "command-without-root", .fault = ManifestFault::CommandWithoutRoot },
    InvalidRow { .file = "duplicate-command", .fault = ManifestFault::Duplicate },
    InvalidRow { .file = "empty-argv", .fault = ManifestFault::EmptyArgv },
    InvalidRow { .file = "empty-head", .fault = ManifestFault::MissingKeyword },
    InvalidRow { .file = "empty-name", .fault = ManifestFault::EmptyName },
    InvalidRow { .file = "env-for-undeclared-command", .fault = ManifestFault::EnvForUndeclared },
    InvalidRow { .file = "path-of-undeclared-root", .fault = ManifestFault::UndeclaredRoot },
    InvalidRow { .file = "placeholder-program", .fault = ManifestFault::PlaceholderProgram },
    InvalidRow { .file = "quoted-word", .fault = ManifestFault::QuotedWord },
    InvalidRow { .file = "read-undeclared-root", .fault = ManifestFault::UndeclaredRoot },
    InvalidRow { .file = "root-out-of-place", .fault = ManifestFault::RootOutOfPlace },
    InvalidRow { .file = "root-unknown-flag", .fault = ManifestFault::UnknownRootFlag },
    InvalidRow { .file = "root-create-over-list", .fault = ManifestFault::CreateOverList },
    InvalidRow { .file = "undeclared-placeholder", .fault = ManifestFault::UndeclaredPlaceholder },
    InvalidRow { .file = "unknown-kind", .fault = ManifestFault::UnknownKind },
    InvalidRow { .file = "unknown-line", .fault = ManifestFault::UnknownLine },
    InvalidRow { .file = "version-1-with-v2-line", .fault = ManifestFault::Version2LineInVersion1 },
    InvalidRow { .file = "version-not-first", .fault = ManifestFault::VersionNotFirst },
    InvalidRow { .file = "forward-reference", .fault = ManifestFault::UndeclaredRoot },
    InvalidRow { .file = "environment-name", .fault = ManifestFault::MalformedEnvironmentName },
    InvalidRow { .file = "root-empty-step", .fault = ManifestFault::MalformedRoot },
    InvalidRow { .file = "root-partial-wildcard", .fault = ManifestFault::MalformedRoot },
    InvalidRow { .file = "tab-in-line", .fault = ManifestFault::TabCharacter },
    InvalidRow { .file = "indented-line", .fault = ManifestFault::SurroundingBlank },
    InvalidRow { .file = "trailing-blank", .fault = ManifestFault::SurroundingBlank },
    InvalidRow { .file = "stray-cr", .fault = ManifestFault::NotUtf8 },
    InvalidRow { .file = "inbox-reserved", .fault = ManifestFault::UnsupportedLine },
    InvalidRow { .file = "title-too-long", .fault = ManifestFault::BadTitle },
    // The whole text is refused before any line is read: a control character never reaches the title.
    InvalidRow { .file = "title-control-character", .fault = ManifestFault::NotUtf8 },
    InvalidRow { .file = "title-repeated", .fault = ManifestFault::Repeated },
    InvalidRow { .file = "pane-input-repeated", .fault = ManifestFault::Repeated },
    InvalidRow { .file = "url-open-repeated", .fault = ManifestFault::Repeated },
    InvalidRow { .file = "title-bidi-override", .fault = ManifestFault::BadTitle },
    InvalidRow { .file = "action-title-too-long", .fault = ManifestFault::BadTitle },
    InvalidRow { .file = "env-set-undeclared-target", .fault = ManifestFault::EnvForUndeclared },
    InvalidRow { .file = "env-set-undeclared-argument", .fault = ManifestFault::UndeclaredPlaceholder },
    InvalidRow { .file = "env-set-argument-not-name", .fault = ManifestFault::EnvSetNotName },
    InvalidRow { .file = "env-set-forbidden-variable", .fault = ManifestFault::ForbiddenEnvironment },
    InvalidRow { .file = "env-set-repeated", .fault = ManifestFault::Duplicate },
    InvalidRow { .file = "env-set-without-placeholder", .fault = ManifestFault::UnknownLine },
};

[[nodiscard]] std::optional<ManifestFault> faultOf(std::string_view text)
{
    auto const parsed = parseManifestText(text);
    return parsed ? std::nullopt : std::optional { parsed.error() };
}

[[nodiscard]] vtextension::DeclaredCommand const* commandNamed(vtextension::Manifest const& manifest,
                                                               std::string_view name)
{
    auto const found = std::ranges::find(manifest.commands, name, &vtextension::DeclaredCommand::name);
    return found != manifest.commands.end() ? &*found : nullptr;
}

} // namespace

TEST_CASE("Manifest v2: every corpus file under valid/ is accepted", "[vtextension][manifest][corpus]")
{
    auto const files = corpusFiles("valid");
    REQUIRE(files.contains("orchestrator"));
    for (auto const& file: files)
    {
        CAPTURE(file);
        auto const parsed =
            parseManifestText(readCorpusFile(corpusDirectory() / "valid" / (file + ".manifest")));
        auto const why = parsed ? std::string_view {} : vtextension::manifestFaultText(parsed.error());
        CAPTURE(why);
        CHECK(parsed.has_value());
    }
}

TEST_CASE("Manifest v2: every corpus file under invalid/ is refused with its fault",
          "[vtextension][manifest][corpus]")
{
    for (auto const& row: InvalidRows)
    {
        CAPTURE(row.file);
        auto const fault =
            faultOf(readCorpusFile(corpusDirectory() / "invalid" / (std::string(row.file) + ".manifest")));
        REQUIRE(fault.has_value());
        CAPTURE(vtextension::manifestFaultText(*fault));
        CHECK(*fault == row.fault);
    }
    // A new corpus file forces a row.
    auto rows = std::set<std::string> {};
    for (auto const& row: InvalidRows)
        rows.emplace(row.file);
    CHECK(corpusFiles("invalid") == rows);
}

TEST_CASE("Manifest v2: a pane and a command bound to a user's set", "[vtextension][manifest]")
{
    using vtextension::EnvironmentSetBinding;

    auto const pane =
        parseManifestText(readCorpusFile(corpusDirectory() / "valid" / "pane-env-set.manifest"));
    REQUIRE(pane.has_value());
    CHECK(pane->panes.front().environmentSet
          == EnvironmentSetBinding { .argument = "env-set",
                                     .variables = { "CLAUDE_CONFIG_DIR", "HTTPS_PROXY" } });
    auto const command =
        parseManifestText(readCorpusFile(corpusDirectory() / "valid" / "command-env-set.manifest"));
    REQUIRE(command.has_value());
    CHECK(command->commands.front().environmentSet->variables
          == std::vector<std::string> { "CLAUDE_CONFIG_DIR", "NO_PROXY" });
    CHECK_FALSE(command->panes.size());
    // A version-1 manifest does not know the lines.
    CHECK(faultOf("name = a\nversion = 1\ncommand x = id\npane-env-set x {a} = NO_PROXY\n")
          == ManifestFault::Version2LineInVersion1);
}

TEST_CASE("Manifest v2: blanks around a line are refused, never trimmed", "[vtextension][manifest]")
{
    // A line of nothing but blanks is a blank line; a CR before the LF is the line end.
    auto const accepted = parseManifestText("name = spaced\r\n   \nversion = 2\r\n#  a comment \n"sv);
    REQUIRE(accepted.has_value());
    CHECK(accepted->name == "spaced");
    // A comment starts in the first column, like every other line.
    CHECK(faultOf("name = x\nversion = 2\n # indented\n"sv) == ManifestFault::SurroundingBlank);
    CHECK(faultOf("name = x\nversion = 2 \n"sv) == ManifestFault::SurroundingBlank);
    CHECK(faultOf("name = x \nversion = 2\n"sv) == ManifestFault::SurroundingBlank);
    // A CR anywhere but right before the LF is still a stray control character.
    CHECK(faultOf("name = x\nversion = 2\r \n"sv) == ManifestFault::NotUtf8);
    // Version 1 keeps trimming.
    CHECK(parseManifestText("  name = x \nversion = 1 \n"sv).has_value());
}

TEST_CASE("Manifest v2: the orchestrator's manifest", "[vtextension][manifest][corpus]")
{
    using vtextension::ArgumentKind;
    using vtextension::CommandTimeout;
    using vtextension::EnvironmentEntry;
    using vtextension::RootCreation;

    auto const text = readCorpusFile(corpusDirectory() / "valid" / "orchestrator.manifest");
    auto const parsed = parseManifestText(text);
    REQUIRE(parsed.has_value());
    auto const& manifest = *parsed;
    // The corpus grows with the extension: the counts are its own lines', every one of them parsed.
    auto const linesStartingWith = [&](std::string_view keyword) {
        return static_cast<std::size_t>(
            std::ranges::count_if(std::views::split(text, '\n'), [&](auto const& line) {
                return std::string_view { line }.starts_with(keyword);
            }));
    };
    CHECK(manifest.name == "orchestrator");
    CHECK(manifest.title == "Agents");
    CHECK(manifest.version == 2);
    CHECK(manifest.placement == vtextension::ManifestPlacement::Daemon);
    CHECK(manifest.digest.empty());

    REQUIRE(manifest.roots.size() == 2);
    CHECK(manifest.roots[0].label == "worktrees");
    CHECK(manifest.roots[0].pointer == "worktree_root");
    CHECK(manifest.roots[0].creation == RootCreation::CreateIfMissing);
    CHECK(manifest.roots[1].label == "repos");
    CHECK(manifest.roots[1].pointer == "repos/*/path");
    CHECK(manifest.roots[1].creation == RootCreation::Existing);
    CHECK(manifest.readableRoots == std::vector<std::string> { "worktrees" });

    REQUIRE(manifest.arguments.size() == linesStartingWith("arg "));
    REQUIRE(manifest.arguments.size() >= 9);
    auto const path = std::ranges::find(manifest.arguments, "path", &vtextension::ArgumentDecl::name);
    REQUIRE(path != manifest.arguments.end());
    CHECK(path->kind == ArgumentKind::Path);
    CHECK(path->root == "worktrees");
    CHECK(std::ranges::all_of(manifest.arguments, [](vtextension::ArgumentDecl const& argument) {
        return argument.kind == ArgumentKind::Path || argument.root.empty();
    }));

    CHECK(manifest.commandEnvironment.size() == 7);
    CHECK(manifest.commands.size() == linesStartingWith("command "));
    CHECK(manifest.commands.size() >= 17);
    auto const* const prs = commandNamed(manifest, "gh-prs");
    REQUIRE(prs != nullptr);
    CHECK(prs->timeout == CommandTimeout::Long);
    CHECK(prs->root == "repos");
    CHECK(prs->environment.empty());
    auto const* const status = commandNamed(manifest, "git-status");
    REQUIRE(status != nullptr);
    CHECK(status->timeout == CommandTimeout::Short);
    CHECK(status->root == "worktrees");
    CHECK(status->environment.size() == 9);
    CHECK(std::ranges::contains(status->environment,
                                EnvironmentEntry { .name = "GIT_CONFIG_COUNT", .value = "3" }));
    CHECK(std::ranges::contains(
        status->environment,
        EnvironmentEntry { .name = "GIT_ATTR_SOURCE", .value = "4b825dc642cb6eb9a060e54bf8d69288fbee4904" }));

    // The agent's tab, and the same without the status board's instruction.
    REQUIRE(manifest.panes.size() == 3);
    for (auto const& pane: manifest.panes | std::views::take(2))
    {
        CHECK(pane.root == "worktrees");
        CHECK(pane.argvTemplate.front() == "claude");
        CHECK(pane.environment.size() == 5);
    }
    // The Copilot worker's tab, which runs the Copilot CLI in the same worktrees with the same git setup.
    CHECK(manifest.panes[2].name == "copilot");
    CHECK(manifest.panes[2].root == "worktrees");
    CHECK(manifest.panes[2].argvTemplate.front() == "copilot");
    CHECK(manifest.panes[2].environment.size() == 5);
    CHECK(manifest.panes[0].name == "agent");
    CHECK(std::ranges::contains(manifest.panes[0].argvTemplate, "{board}"));
    CHECK(manifest.panes[1].name == "agent-plain");
    CHECK_FALSE(std::ranges::contains(manifest.panes[1].argvTemplate, "{board}"));

    CHECK(manifest.servedScheme == "orchestrator");
    CHECK(manifest.actions.size() == 3);
    CHECK(manifest.control.empty());
}

TEST_CASE("Manifest v2: a version-2 line is refused under version 1", "[vtextension][manifest]")
{
    auto const line = GENERATE(as<std::string_view> {},
                               "placement daemon",
                               "root r = config:x",
                               "root r create = config:x",
                               "read r",
                               "arg a = name",
                               "command c short @r = git",
                               "command-env A = 1",
                               "command-env-for c A = 1",
                               "pane p @r = sh",
                               "pane-env p A = 1",
                               "pane-env-set p {a} = NO_PROXY",
                               "command-env-set c {a} = NO_PROXY",
                               "inbox i = text",
                               "serve s",
                               "action a = Title",
                               "title = Title");
    CAPTURE(line);
    CHECK(faultOf(std::format("name = x\nversion = 1\n{}\n", line)) == ManifestFault::Version2LineInVersion1);
}

TEST_CASE("Manifest v2: title is a version-2 line", "[vtextension][manifest]")
{
    auto const titled = parseManifestText(
        "name = agents\nversion = 2\ntitle = Agents \xC2\xB7 all forty characters of it, ok!\n"sv);
    REQUIRE(titled.has_value());
    CHECK(titled->title == "Agents \xC2\xB7 all forty characters of it, ok!");
    CHECK(vtextension::displayTitleOf(*titled) == titled->title);
    CHECK(faultOf("name = agents\nversion = 1\ntitle = Agents\n"sv) == ManifestFault::Version2LineInVersion1);
    CHECK(faultOf("name = agents\ntitle = Agents\nversion = 2\n"sv) == ManifestFault::VersionNotFirst);
    CHECK(faultOf("name = agents\nversion = 2\ntitle =\n"sv) == ManifestFault::EmptyName);
    CHECK(faultOf("name = agents\nversion = 2\ntitle x = Agents\n"sv) == ManifestFault::UnknownLine);

    CHECK(faultOf("name = agents\nversion = 2\ntitle = Agents \xC2\xB7 one character beyond forty, sad!\n"sv)
          == ManifestFault::BadTitle);
    CHECK(faultOf("name = agents\nversion = 2\ntitle = Agents\ntitle = Others\n"sv)
          == ManifestFault::Repeated);

    // What would disguise a title in the chrome is refused, in an action's title too, as is a long one.
    CHECK(faultOf("name = agents\nversion = 2\ntitle = Age\xE2\x80\xAEnts\n"sv) == ManifestFault::BadTitle);
    CHECK(faultOf("name = agents\nversion = 2\ntitle = Age\xE2\x80\x8B"
                  "nts\n"sv)
          == ManifestFault::BadTitle);
    CHECK(faultOf("name = agents\nversion = 2\ntitle = Agents\xE2\x80\xA8\n"sv) == ManifestFault::BadTitle);
    CHECK(faultOf("name = agents\nversion = 2\naction open = Open \xE2\x81\xA6"
                  "Agents\n"sv)
          == ManifestFault::BadTitle);
    CHECK(
        faultOf(
            "name = agents\nversion = 2\naction open = Open Agents \xC2\xB7 one character too many, ok!\n"sv)
        == ManifestFault::BadTitle);
    CHECK(parseManifestText(
              "name = agents\nversion = 2\naction open = Open Agents \xC2\xB7 exactly forty characters!!\n"sv)
              .has_value());

    SECTION("absent, the name stands in")
    {
        auto const untitled = parseManifestText("name = agents\nversion = 2\n"sv);
        REQUIRE(untitled.has_value());
        CHECK(untitled->title.empty());
        CHECK(vtextension::displayTitleOf(*untitled) == untitled->name);
    }
}

TEST_CASE("Manifest v1: unchanged", "[vtextension][manifest]")
{
    auto const parsed = parseManifestText("name = x\nversion = 1\ncommand st = git status");
    REQUIRE(parsed.has_value());
    CHECK(parsed->version == 1);
    CHECK(parsed->placement == vtextension::ManifestPlacement::StatusLine);
    REQUIRE(parsed->commands.size() == 1);
    CHECK(parsed->commands[0].timeout == vtextension::CommandTimeout::Short);
    CHECK(parsed->commands[0].root.empty());
    CHECK(parsed->commands[0].environment.empty());
    CHECK(parsed->roots.empty());
}

TEST_CASE("Manifest v2: the faults of the line grammar", "[vtextension][manifest]")
{
    struct Row
    {
        std::string_view body; ///< What follows `name = x` and `version = 2`.
        std::optional<ManifestFault> fault;
    };
    constexpr auto Rooted = "root r = config:x\n"sv;
    auto const rows = std::vector<Row> {
        { .body = "", .fault = std::nullopt },
        { .body = "placement status-line", .fault = std::nullopt },
        { .body = "placement daemon\nplacement daemon", .fault = ManifestFault::Repeated },
        { .body = "placement everywhere", .fault = ManifestFault::UnknownLine },
        { .body = "serve a\nserve b", .fault = ManifestFault::Repeated },
        { .body = "root r = x", .fault = ManifestFault::MalformedRoot },
        { .body = "root r = config:", .fault = ManifestFault::MalformedRoot },
        { .body = "root r = config:a//b", .fault = ManifestFault::MalformedRoot },
        { .body = "root r = config:a/b*/c", .fault = ManifestFault::MalformedRoot },
        { .body = "root r = config:a\nroot r = config:b", .fault = ManifestFault::Duplicate },
        { .body = "root r create extra = config:a", .fault = ManifestFault::UnknownLine },
        { .body = "command c short @s = git", .fault = ManifestFault::UndeclaredRoot },
        { .body = "command c sometimes @r = git", .fault = ManifestFault::UnknownTimeout },
        { .body = "command c @r short = git", .fault = ManifestFault::RootOutOfPlace },
        { .body = "command c short @r extra = git", .fault = ManifestFault::UnknownLine },
        { .body = "command c short @r = git  status", .fault = ManifestFault::UnknownLine },
        { .body = "command c short @r = git sta'tus", .fault = ManifestFault::QuotedWord },
        { .body = "pane p = sh", .fault = ManifestFault::CommandWithoutRoot },
        { .body = "pane p @r =", .fault = ManifestFault::EmptyArgv },
        { .body = "arg a = name\narg a = ref", .fault = ManifestFault::Duplicate },
        { .body = "arg a = path:", .fault = ManifestFault::UnknownKind },
        { .body = "command-env A = 1\ncommand-env A = 2", .fault = ManifestFault::Duplicate },
        { .body = "command-env A=B = 1", .fault = ManifestFault::MalformedEnvironmentName },
        { .body = "command-env 1A = 1", .fault = ManifestFault::MalformedEnvironmentName },
        { .body = "command-env\tA = 1", .fault = ManifestFault::TabCharacter },
        { .body = "inbox i = text", .fault = ManifestFault::UnsupportedLine },
        { .body = "command c short @r = git {a}\narg a = name",
          .fault = ManifestFault::UndeclaredPlaceholder },
        { .body = "command-env-for c A = 1\ncommand c short @r = git",
          .fault = ManifestFault::EnvForUndeclared },
        { .body = "arg p = path:q\nroot q = config:y", .fault = ManifestFault::UndeclaredRoot },
        { .body = "command-env A =", .fault = ManifestFault::EmptyName },
        { .body = "pane-env p A = 1", .fault = ManifestFault::EnvForUndeclared },
        { .body = "action a = One\naction a = Two", .fault = ManifestFault::Duplicate },
        { .body = "control frobnicate", .fault = ManifestFault::UnknownPermission },
        { .body = "control read\ncontrol read", .fault = ManifestFault::UnknownPermission },
        { .body = "name = y", .fault = ManifestFault::Repeated },
    };
    for (auto const& row: rows)
    {
        auto const text = std::format("name = x\nversion = 2\n{}{}\n", Rooted, row.body);
        CAPTURE(text);
        CHECK(faultOf(text) == row.fault);
    }
    // A name is declared before it is used.
    CHECK_FALSE(
        faultOf("name = x\nversion = 2\nroot r = config:x\narg a = name\ncommand c short @r = git {a}\n"));
    CHECK(faultOf("name = x\nversion = 2\ncommand c short @r = git\nroot r = config:x\n")
          == ManifestFault::UndeclaredRoot);
    // Version 2 wants its version line before every line but the name; version 1 takes any order, as before.
    CHECK_FALSE(faultOf("version = 2\nname = x\n"));
    CHECK(faultOf("name = x\nserve s\nversion = 2\n") == ManifestFault::VersionNotFirst);
    CHECK_FALSE(faultOf("command c = git\nname = x\nversion = 1\n"));
    // Version 1 still reads a tab as a blank.
    CHECK_FALSE(faultOf("name = x\nversion = 1\ncommand c =\tgit\tstatus\n"));
    CHECK(faultOf("version = 2\n") == ManifestFault::MissingName);
    CHECK(faultOf("name = x\n") == ManifestFault::MissingVersion);
    CHECK(faultOf("name = x\nversion = 3\n") == ManifestFault::UnsupportedVersion);
    CHECK(faultOf("name = x\nversion = 2\nversion = 2\n") == ManifestFault::Repeated);
    CHECK(faultOf("name = x\nversion = 2\n\x01\n") == ManifestFault::NotUtf8);
}

TEST_CASE("Manifest: every fault has a text", "[vtextension][manifest]")
{
    for (auto const index:
         std::views::iota(uint8_t { 0 }, static_cast<uint8_t>(vtextension::ManifestFaultTexts.size())))
    {
        auto const fault = static_cast<ManifestFault>(index);
        CHECK_FALSE(vtextension::manifestFaultText(fault).empty());
    }
}

TEST_CASE("Manifest v2: parseManifest refuses any fault as ManifestInvalid", "[vtextension][manifest]")
{
    auto const result = parseManifest(moduleWith("name = x\nversion = 2\nread nowhere\n"));
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error() == ExtensionError::ManifestInvalid);
}

TEST_CASE("Manifest: the digest covers the whole section", "[vtextension][manifest]")
{
    constexpr auto Head = "name = x\nversion = 2\nplacement daemon\n"sv;
    auto const one = parseManifest(moduleWith(std::format("{}action open = Open Agents\n", Head)));
    auto const two = parseManifest(moduleWith(std::format("{}action open = Open agents\n", Head)));
    REQUIRE(one.has_value());
    REQUIRE(two.has_value());
    REQUIRE(one->actions.size() == 1);
    REQUIRE(two->actions.size() == 1);
    CHECK(one->actions[0].title != two->actions[0].title);
    CHECK(one->digest != two->digest);
}

// }}}

TEST_CASE("Manifest: the digest covers the whole module, its code too", "[vtextension][manifest]")
{
    // The same manifest over different code is another module, and is asked about anew.
    auto const plain = moduleWith(Example);
    auto withCode = plain;
    // A custom section `x` holding one byte: code the manifest says nothing about.
    for (auto const byte: { 0x00, 0x03, 0x01, 0x78, 0x2A })
        withCode.push_back(static_cast<std::byte>(byte));
    auto const one = parseManifest(plain);
    auto const two = parseManifest(withCode);
    REQUIRE(one.has_value());
    REQUIRE(two.has_value());
    CHECK(one->name == two->name);
    CHECK(one->commands.size() == two->commands.size());
    CHECK(one->digest != two->digest);
}

TEST_CASE("Manifest v2: url-open lets url-open ask the frontend to open links", "[vtextension][manifest]")
{
    constexpr auto Head = "name = p\nversion = 2\nplacement daemon\n";
    auto const without = parseManifestText(Head);
    REQUIRE(without.has_value());
    CHECK(without->urlOpening == vtextension::UrlOpening::Refused);
    auto const with = parseManifestText(std::string { Head } + "url-open\n");
    REQUIRE(with.has_value());
    CHECK(with->urlOpening == vtextension::UrlOpening::Allowed);
    CHECK(parseManifestText(std::string { Head } + "url-open\nurl-open\n").error()
          == ManifestFault::Repeated);
    CHECK(parseManifestText(std::string { Head } + "url-open https\n").error() == ManifestFault::UnknownLine);
}

TEST_CASE("Manifest v2: pane-input lets pane-send type into the panes", "[vtextension][manifest]")
{
    constexpr auto Head =
        "name = p\nversion = 2\nroot w = config:w\narg prompt = text\npane agent @w = claude {prompt}\n";
    auto const without = parseManifestText(Head);
    REQUIRE(without.has_value());
    CHECK(without->paneInput == vtextension::PaneInput::Refused);
    auto const with = parseManifestText(std::string { Head } + "pane-input\n");
    REQUIRE(with.has_value());
    CHECK(with->paneInput == vtextension::PaneInput::Typed);
    CHECK(parseManifestText(std::string { Head } + "pane-input agent\n").error()
          == ManifestFault::UnknownLine);
    CHECK(parseManifestText(std::string { Head } + "pane-input = x\n").error() == ManifestFault::UnknownLine);
}
