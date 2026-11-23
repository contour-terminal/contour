// SPDX-License-Identifier: Apache-2.0
#include <vtpty/PathContainment.hpp>
#include <vtpty/PathStyle.hpp>
#include <vtpty/WindowsPathRules.hpp>

#include <core/Utf8Path.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <vtextension/RootPathSyntax.hpp>

using namespace std::string_literals;
using namespace std::string_view_literals;
using vtextension::childPath;
using vtextension::comparisonKeyOf;
using vtextension::componentsBeneath;
using vtextension::hasAbsoluteRoot;
using vtextension::isAtOrBeneath;
using vtextension::isNormalisedAbsolutePath;
using vtextension::isPlainComponent;
using vtextension::isVolumeRoot;
using vtextension::MaxPathArgumentBytes;
using vtextension::parentAndLeaf;
using vtextension::samePath;
using vtpty::PathStyle;

namespace
{

enum class Verdict : uint8_t
{
    Refused,
    Accepted,
};

struct PathRow
{
    std::string path;
    Verdict expected;
};

/// How a containment row's verdict relates to vtpty::isAtOrBeneath(), the filesystem-path rule.
enum class NativeRule : uint8_t
{
    Agrees,   ///< Both rules give the same verdict; the native-grammar cross-check compares them.
    Stricter, ///< A fail-closed row: this rule refuses a spelling the filesystem-path rule may admit.
};

struct ContainmentRow
{
    std::string_view path;
    std::string_view directory;
    PathStyle style;
    Verdict expected;
    NativeRule native = NativeRule::Agrees;
};

/// A Windows row this rule refuses beneath `C:\w`, whatever the filesystem-path rule says of it.
[[nodiscard]] constexpr ContainmentRow failsClosedBeneathW(std::string_view path) noexcept
{
    return { .path = path,
             .directory = R"(C:\w)",
             .style = PathStyle::Windows,
             .expected = Verdict::Refused,
             .native = NativeRule::Stricter };
}

using Split = std::optional<std::pair<std::string_view, std::string_view>>;
using Components = std::optional<std::vector<std::string_view>>;

/// The containment rows, both grammars; the native-grammar cross-check reuses them.
constexpr auto ContainmentRows = std::to_array<ContainmentRow>({
    { .path = "/w/a", .directory = "/w", .style = PathStyle::Posix, .expected = Verdict::Accepted },
    { .path = "/wx", .directory = "/w", .style = PathStyle::Posix, .expected = Verdict::Refused },
    { .path = "/", .directory = "/", .style = PathStyle::Posix, .expected = Verdict::Accepted },
    { .path = "/W", .directory = "/w", .style = PathStyle::Posix, .expected = Verdict::Refused },
    { .path = R"(C:\W\a)",
      .directory = R"(C:\w)",
      .style = PathStyle::Windows,
      .expected = Verdict::Accepted },
    { .path = R"(C:\wx)", .directory = R"(C:\w)", .style = PathStyle::Windows, .expected = Verdict::Refused },
    { .path = R"(D:\w)", .directory = R"(C:\w)", .style = PathStyle::Windows, .expected = Verdict::Refused },
    { .path = R"(C:\x)", .directory = R"(C:\)", .style = PathStyle::Windows, .expected = Verdict::Accepted },
    { .path = R"(\\SRV\Share\x)",
      .directory = R"(\\srv\share)",
      .style = PathStyle::Windows,
      .expected = Verdict::Accepted },
    { .path = R"(\\srv\other)",
      .directory = R"(\\srv\share)",
      .style = PathStyle::Windows,
      .expected = Verdict::Refused },
    { .path = "C:\\\xc3\x84\\x",   // C:\Ä\x
      .directory = "C:\\\xc3\xa4", // C:\ä
      .style = PathStyle::Windows,
      .expected = Verdict::Refused },
    // A root nested in another: an ASCII case variant is inside it; a non-ASCII one is not (only ASCII
    // folds).
    { .path = R"(C:\WT\INNER\x)",
      .directory = R"(C:\wt\inner)",
      .style = PathStyle::Windows,
      .expected = Verdict::Accepted },
    { .path = "C:\\wt\\\xc3\xa4\\x",   // C:\wt\ä\x
      .directory = "C:\\wt\\\xc3\x84", // C:\wt\Ä
      .style = PathStyle::Windows,
      .expected = Verdict::Refused },
    failsClosedBeneathW(R"(C:\w\x.)"),
    failsClosedBeneathW(R"(C:\w\x )"),
    failsClosedBeneathW(R"(C:\w\con)"),
    failsClosedBeneathW(R"(C:\w\x:y)"),
    failsClosedBeneathW(R"(C:\w/x)"),
    failsClosedBeneathW(R"(C:\w\x\..\y)"),
    failsClosedBeneathW(R"(\\?\C:\w\x)"),
    failsClosedBeneathW(R"(C:\w\)"),
});

/// One row of a shared corpus in extensions/orchestrator/contract/windows-paths.
struct CorpusRow
{
    std::string value; ///< The value, its `%XX` escapes decoded.
    Verdict expected;  ///< Whether the grammar accepts it.
};

/// @return @p text with every `%XX` replaced by the byte it names.
[[nodiscard]] std::string percentDecoded(std::string_view text)
{
    auto decoded = std::string {};
    while (!text.empty())
    {
        if (text.front() != '%')
        {
            decoded += text.front();
            text.remove_prefix(1);
            continue;
        }
        REQUIRE(text.size() >= 3);
        auto byte = 0U;
        auto const parsed = std::from_chars(text.data() + 1, text.data() + 3, byte, 16);
        REQUIRE((parsed.ec == std::errc {} && parsed.ptr == text.data() + 3));
        decoded += static_cast<char>(byte);
        text.remove_prefix(3);
    }
    return decoded;
}

/// Reads a corpus file the guest's tests read too: one `value<TAB>verdict` row per line, `#` comments.
/// @param file The file's name in the corpus directory.
/// @return Its rows, in order.
[[nodiscard]] std::vector<CorpusRow> windowsCorpus(std::string_view file)
{
    auto stream = std::ifstream { std::filesystem::path { VTEXTENSION_WINDOWS_PATHS_CORPUS_DIR } / file,
                                  std::ios::binary };
    REQUIRE(stream.is_open());
    auto const text =
        std::string { std::istreambuf_iterator<char> { stream }, std::istreambuf_iterator<char> {} };
    auto rows = std::vector<CorpusRow> {};
    for (auto const& range: std::views::split(text, '\n'))
    {
        auto line = std::string_view { range.begin(), range.end() };
        if (line.ends_with('\r'))
            line.remove_suffix(1);
        if (line.empty() || line.starts_with('#'))
            continue;
        auto const tab = line.find('\t');
        REQUIRE(tab != std::string_view::npos);
        auto const verdict = line.substr(tab + 1);
        REQUIRE((verdict == "accept" || verdict == "refuse"));
        rows.push_back({ .value = percentDecoded(line.substr(0, tab)),
                         .expected = verdict == "accept" ? Verdict::Accepted : Verdict::Refused });
    }
    REQUIRE_FALSE(rows.empty());
    return rows;
}

/// An absolute POSIX path of exactly @p bytes bytes: `/` and then `a`s.
[[nodiscard]] std::string posixPathOf(std::size_t bytes)
{
    return "/" + std::string(bytes - 1, 'a');
}

/// An absolute Windows path of exactly @p bytes bytes: `C:\` and then `a`s.
[[nodiscard]] std::string windowsPathOf(std::size_t bytes)
{
    return R"(C:\)" + std::string(bytes - 3, 'a');
}

} // namespace

TEST_CASE("RootPathSyntax: normalised absolute paths, POSIX", "[vtextension][roots]")
{
    using enum Verdict;
    auto const rows = std::vector<PathRow> {
        { "/home/u/worktrees/c/pr-1", Accepted },
        { posixPathOf(MaxPathArgumentBytes), Accepted },
        { "rel/x", Refused },
        { "/a/../b", Refused },
        { "/a/./b", Refused },
        { "/a//b", Refused },
        { "/a\0b"s, Refused },
        { posixPathOf(MaxPathArgumentBytes + 1), Refused },
        { R"(C:\x)", Refused },
    };
    for (auto const& row: rows)
    {
        CAPTURE(row.path);
        CHECK(isNormalisedAbsolutePath(row.path, PathStyle::Posix) == (row.expected == Accepted));
    }
    CHECK(hasAbsoluteRoot("/x", PathStyle::Posix));
    CHECK_FALSE(hasAbsoluteRoot(R"(C:\x)", PathStyle::Posix));
    CHECK(isVolumeRoot("/", PathStyle::Posix));
    CHECK_FALSE(isVolumeRoot("/x", PathStyle::Posix));
    CHECK(isPlainComponent("a b", PathStyle::Posix));
    CHECK(isPlainComponent(R"(a\b)", PathStyle::Posix));
    CHECK_FALSE(isPlainComponent("", PathStyle::Posix));
    CHECK_FALSE(isPlainComponent(".", PathStyle::Posix));
    CHECK_FALSE(isPlainComponent("..", PathStyle::Posix));
    CHECK_FALSE(isPlainComponent("a/b", PathStyle::Posix));
    CHECK_FALSE(isPlainComponent("a\0b"sv, PathStyle::Posix));
}

TEST_CASE("RootPathSyntax: normalised absolute paths, Windows", "[vtextension][roots]")
{
    using enum Verdict;
    auto const rows = std::vector<PathRow> {
        { R"(C:\)", Accepted },
        { R"(c:\x)", Accepted },
        { R"(C:\Users\me\wt\contour\0193)", Accepted },
        { R"(\\srv\share)", Accepted },
        { R"(\\srv\share\x)", Accepted },
        { R"(C:\PROGRA~1\x)", Accepted },
        { windowsPathOf(MaxPathArgumentBytes), Accepted },
        { "/x", Refused },
        { "C:", Refused },
        { "C:x", Refused },
        { R"(\x)", Refused },
        { "C:/x", Refused },
        { R"(C:\x/y)", Refused },
        { R"(C:\x\)", Refused },
        { R"(C:\\x)", Refused },
        { R"(C:\x\..\y)", Refused },
        { R"(C:\x\.\y)", Refused },
        { R"(C:\x:y)", Refused },
        { R"(C:\con)", Refused },
        { R"(C:\nul.txt)", Refused },
        { R"(C:\x.)", Refused },
        { R"(C:\x )", Refused },
        { R"(\\srv)", Refused },
        { R"(\\srv\)", Refused },
        { R"(\\?\C:\x)", Refused },
        { R"(\\.\pipe\x)", Refused },
        { R"(\\srv\share\)", Refused },
        { windowsPathOf(MaxPathArgumentBytes + 1), Refused },
        { "C:\\a\0b"s, Refused },
    };
    for (auto const& row: rows)
    {
        CAPTURE(row.path);
        CHECK(isNormalisedAbsolutePath(row.path, PathStyle::Windows) == (row.expected == Accepted));
    }
    CHECK(hasAbsoluteRoot(R"(C:\x)", PathStyle::Windows));
    CHECK(hasAbsoluteRoot(R"(\\srv\share)", PathStyle::Windows));
    CHECK_FALSE(hasAbsoluteRoot("/x", PathStyle::Windows));
    CHECK_FALSE(hasAbsoluteRoot("C:x", PathStyle::Windows));
    CHECK_FALSE(hasAbsoluteRoot(R"(\x)", PathStyle::Windows));
    CHECK(isVolumeRoot(R"(C:\)", PathStyle::Windows));
    CHECK(isVolumeRoot(R"(\\srv\share)", PathStyle::Windows));
    CHECK_FALSE(isVolumeRoot(R"(C:\x)", PathStyle::Windows));
    CHECK_FALSE(isVolumeRoot(R"(\\srv\share\x)", PathStyle::Windows));
    CHECK_FALSE(isVolumeRoot(R"(\\srv)", PathStyle::Windows));
    CHECK_FALSE(isVolumeRoot("/", PathStyle::Windows));
    CHECK(isPlainComponent("a b", PathStyle::Windows));
    CHECK_FALSE(isPlainComponent(R"(a\b)", PathStyle::Windows));
    CHECK_FALSE(isPlainComponent("a/b", PathStyle::Windows));
    CHECK_FALSE(isPlainComponent("..", PathStyle::Windows));
    CHECK_FALSE(isPlainComponent("CON", PathStyle::Windows));
}

TEST_CASE("RootPathSyntax: containment and equality", "[vtextension][roots]")
{
    for (auto const& row: ContainmentRows)
    {
        CAPTURE(row.path, row.directory, row.style);
        CHECK(isAtOrBeneath(row.path, row.directory, row.style) == (row.expected == Verdict::Accepted));
    }
    CHECK(samePath(R"(C:\WT)", R"(c:\wt)", PathStyle::Windows));
    CHECK_FALSE(samePath("/WT", "/wt", PathStyle::Posix));
    CHECK(samePath("/wt", "/wt", PathStyle::Posix));
    CHECK_FALSE(samePath("C:\\\xc3\x84", "C:\\\xc3\xa4", PathStyle::Windows));
    CHECK(comparisonKeyOf(R"(C:\WT)", PathStyle::Windows) == R"(c:\wt)");
    CHECK(comparisonKeyOf("/WT", PathStyle::Posix) == "/WT");
    CHECK(comparisonKeyOf("C:\\\xc3\x84", PathStyle::Windows) == "c:\\\xc3\x84");
}

TEST_CASE("RootPathSyntax: join and split", "[vtextension][roots]")
{
    CHECK(childPath(R"(C:\)", "x", PathStyle::Windows) == R"(C:\x)");
    CHECK(childPath(R"(C:\a)", "x", PathStyle::Windows) == R"(C:\a\x)");
    CHECK(childPath(R"(\\s\sh)", "x", PathStyle::Windows) == R"(\\s\sh\x)");
    CHECK(childPath("/", "x", PathStyle::Posix) == "/x");
    CHECK(childPath("/a", "x", PathStyle::Posix) == "/a/x");

    CHECK(parentAndLeaf(R"(C:\a)", PathStyle::Windows) == Split { std::pair { R"(C:\)"sv, "a"sv } });
    CHECK(parentAndLeaf(R"(C:\a\b)", PathStyle::Windows) == Split { std::pair { R"(C:\a)"sv, "b"sv } });
    CHECK(parentAndLeaf(R"(\\s\sh\a)", PathStyle::Windows) == Split { std::pair { R"(\\s\sh)"sv, "a"sv } });
    CHECK(parentAndLeaf(R"(C:\)", PathStyle::Windows) == std::nullopt);
    CHECK(parentAndLeaf(R"(\\s\sh)", PathStyle::Windows) == std::nullopt);
    CHECK(parentAndLeaf("/a", PathStyle::Posix) == Split { std::pair { "/"sv, "a"sv } });
    CHECK(parentAndLeaf("/a/b", PathStyle::Posix) == Split { std::pair { "/a"sv, "b"sv } });
    CHECK(parentAndLeaf("/", PathStyle::Posix) == std::nullopt);

    CHECK(componentsBeneath(R"(C:\wt)", R"(c:\WT\r\x)", PathStyle::Windows)
          == Components { std::vector { "r"sv, "x"sv } });
    CHECK(componentsBeneath(R"(C:\wt)", R"(C:\wt)", PathStyle::Windows) == std::nullopt);
    CHECK(componentsBeneath(R"(C:\)", R"(C:\a)", PathStyle::Windows) == Components { std::vector { "a"sv } });
    CHECK(componentsBeneath(R"(C:\wt)", R"(C:\wtx)", PathStyle::Windows) == std::nullopt);
    CHECK(componentsBeneath("/wt", "/wt/r/x", PathStyle::Posix)
          == Components { std::vector { "r"sv, "x"sv } });
    CHECK(componentsBeneath("/wt", "/WT/r", PathStyle::Posix) == std::nullopt);
    CHECK(componentsBeneath("/wt", "/wt/../x", PathStyle::Posix) == std::nullopt);
}

TEST_CASE("RootPathSyntax: agrees with vtpty::isAtOrBeneath in the native grammar", "[vtextension][roots]")
{
    auto checked = 0;
    for (auto const& row: ContainmentRows)
    {
        if (row.style != vtpty::NativePathStyle || row.native != NativeRule::Agrees)
            continue;
        CAPTURE(row.path, row.directory);
        CHECK(isAtOrBeneath(row.path, row.directory, vtpty::NativePathStyle)
              == vtpty::isAtOrBeneath(core::pathFromTrustedUtf8(row.path),
                                      core::pathFromTrustedUtf8(row.directory),
                                      vtpty::NativePathCase));
        ++checked;
    }
    CHECK(checked > 0);
}

TEST_CASE("RootPathSyntax: the Windows grammar agrees with the guest's corpus", "[vtextension][roots]")
{
    for (auto const& row: windowsCorpus("names.txt"))
    {
        CAPTURE(row.value);
        CHECK(vtpty::isPlainWindowsName(row.value) == (row.expected == Verdict::Accepted));
    }
    for (auto const& row: windowsCorpus("paths.txt"))
    {
        CAPTURE(row.value);
        CHECK(isNormalisedAbsolutePath(row.value, PathStyle::Windows) == (row.expected == Verdict::Accepted));
    }
}
