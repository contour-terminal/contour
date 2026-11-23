// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <orchestrator/core/ArgumentKinds.hpp>
#include <orchestrator/core/Fixtures.hpp>

using namespace std::string_view_literals;

namespace
{

using orchestrator::argumentAccepts;
using orchestrator::ArgumentKind;

enum class Verdict : uint8_t
{
    Refused,
    Accepted,
};

struct AcceptRow
{
    ArgumentKind kind;
    std::string_view value;
    Verdict expected;
};

/// @p N copies of @p c, as storage a row's string_view can point into.
template <std::size_t N>
constexpr auto repeated(char c)
{
    auto text = std::array<char, N> {};
    text.fill(c);
    return text;
}

/// The array's characters as a string_view.
template <std::size_t N>
constexpr auto viewOf(std::array<char, N> const& text)
{
    return std::string_view { text.data(), text.size() };
}

constexpr auto Chars64 = repeated<64>('a');
constexpr auto Chars65 = repeated<65>('a');
constexpr auto Chars200 = repeated<200>('a');
constexpr auto Chars201 = repeated<201>('a');
constexpr auto Chars8192 = repeated<8192>('a');
constexpr auto Chars8193 = repeated<8193>('a');

/// An absolute path of @p N bytes: '/' and one long component.
template <std::size_t N>
constexpr auto absolutePath()
{
    auto path = repeated<N>('a');
    path.front() = '/';
    return path;
}

constexpr auto Path4096 = absolutePath<orchestrator::MaxPathBytes>();
constexpr auto Path4097 = absolutePath<orchestrator::MaxPathBytes + 1>();

constexpr auto AcceptRows = std::array {
    AcceptRow { .kind = ArgumentKind::Number, .value = "2334", .expected = Verdict::Accepted },
    AcceptRow { .kind = ArgumentKind::Number, .value = "1234567890", .expected = Verdict::Accepted },
    AcceptRow { .kind = ArgumentKind::Number, .value = "", .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Number, .value = "-1", .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Number, .value = "12345678901", .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Number, .value = "12a", .expected = Verdict::Refused },

    AcceptRow { .kind = ArgumentKind::Name, .value = "origin", .expected = Verdict::Accepted },
    AcceptRow { .kind = ArgumentKind::Name, .value = "claude-opus-5-5", .expected = Verdict::Accepted },
    AcceptRow { .kind = ArgumentKind::Name, .value = viewOf(Chars64), .expected = Verdict::Accepted },
    AcceptRow { .kind = ArgumentKind::Name, .value = "", .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Name, .value = "ext::sh", .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Name, .value = "-x", .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Name, .value = ".hidden", .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Name, .value = "a/b", .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Name, .value = "https://evil", .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Name, .value = viewOf(Chars65), .expected = Verdict::Refused },

    AcceptRow { .kind = ArgumentKind::Ref, .value = "origin/master", .expected = Verdict::Accepted },
    AcceptRow {
        .kind = ArgumentKind::Ref, .value = "agent/pr-1-20261113-141207", .expected = Verdict::Accepted },
    AcceptRow { .kind = ArgumentKind::Ref, .value = "user@branch", .expected = Verdict::Accepted },
    AcceptRow { .kind = ArgumentKind::Ref, .value = "fix#12", .expected = Verdict::Accepted },
    AcceptRow { .kind = ArgumentKind::Ref, .value = "c++", .expected = Verdict::Accepted },
    AcceptRow { .kind = ArgumentKind::Ref, .value = viewOf(Chars200), .expected = Verdict::Accepted },
    AcceptRow { .kind = ArgumentKind::Ref, .value = "", .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Ref, .value = "a..b", .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Ref, .value = "a//b", .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Ref, .value = "-a", .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Ref, .value = "/a", .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Ref, .value = "a b", .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Ref, .value = "a:b", .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Ref, .value = "a~1", .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Ref, .value = "a@{1}", .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Ref, .value = viewOf(Chars201), .expected = Verdict::Refused },

    AcceptRow {
        .kind = ArgumentKind::Path, .value = "/home/u/worktrees/c/pr-1", .expected = Verdict::Accepted },
    AcceptRow { .kind = ArgumentKind::Path, .value = viewOf(Path4096), .expected = Verdict::Accepted },
    AcceptRow { .kind = ArgumentKind::Path, .value = viewOf(Path4097), .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Path, .value = "", .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Path, .value = "/", .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Path, .value = "rel/x", .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Path, .value = "/a/../b", .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Path, .value = "/a/./b", .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Path, .value = "/a//b", .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Path, .value = "/a/b/", .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Path, .value = "/a/..", .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Path, .value = "/a\0b"sv, .expected = Verdict::Refused },
    // A Windows host's paths, as it spells them.
    AcceptRow { .kind = ArgumentKind::Path, .value = R"(C:\wt\contour\r1)", .expected = Verdict::Accepted },
    AcceptRow { .kind = ArgumentKind::Path, .value = R"(\\s\sh\x)", .expected = Verdict::Accepted },
    AcceptRow { .kind = ArgumentKind::Path, .value = R"(\\s\sh)", .expected = Verdict::Accepted },
    AcceptRow { .kind = ArgumentKind::Path, .value = R"(C:\)", .expected = Verdict::Accepted },
    AcceptRow { .kind = ArgumentKind::Path, .value = R"(C:\wt\)", .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Path, .value = R"(C:\a\..\b)", .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Path, .value = R"(C:\a\.\b)", .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Path, .value = R"(C:\a\\b)", .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Path, .value = "C:/a", .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Path, .value = R"(C:\a/b)", .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Path, .value = "C:a", .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Path, .value = R"(\x)", .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Path, .value = R"(\\s)", .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Path, .value = R"(\\s\)", .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Path, .value = R"(\\?\C:\a)", .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Path, .value = R"(\\.\COM1)", .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Path, .value = R"(C:\repos.)", .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Path, .value = R"(C:\repos )", .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Path, .value = R"(C:\repos:x)", .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Path, .value = R"(C:\con)", .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Path, .value = R"(C:\Nul.txt)", .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Path, .value = "C:\\LPT\xc2\xb9", .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Path, .value = R"(C:\a|b)", .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Path, .value = "C:\\a\tb", .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Path, .value = R"(C:\console)", .expected = Verdict::Accepted },

    AcceptRow { .kind = ArgumentKind::Uuid,
                .value = "550e8400-e29b-41d4-a716-446655440000",
                .expected = Verdict::Accepted },
    AcceptRow { .kind = ArgumentKind::Uuid,
                .value = "550E8400-E29B-41D4-A716-446655440000",
                .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Uuid, .value = "550e8400", .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Uuid,
                .value = "550e8400-e29b-41d4-a716-44665544000g",
                .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Uuid,
                .value = "550e8400e-29b-41d4-a716-446655440000",
                .expected = Verdict::Refused },

    AcceptRow { .kind = ArgumentKind::Text, .value = "assignee:@me", .expected = Verdict::Accepted },
    AcceptRow {
        .kind = ArgumentKind::Text, .value = "Review this.\nThen report.", .expected = Verdict::Accepted },
    AcceptRow { .kind = ArgumentKind::Text, .value = viewOf(Chars8192), .expected = Verdict::Accepted },
    AcceptRow { .kind = ArgumentKind::Text, .value = "-label:x", .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Text, .value = "a\0b"sv, .expected = Verdict::Refused },
    AcceptRow { .kind = ArgumentKind::Text, .value = viewOf(Chars8193), .expected = Verdict::Refused },
};

// The judgement is usable where the host needs it at compile time, too.
static_assert(argumentAccepts(ArgumentKind::Number, "2334"));
static_assert(argumentAccepts(ArgumentKind::Uuid, "550e8400-e29b-41d4-a716-446655440000"));
static_assert(!argumentAccepts(ArgumentKind::Ref, "a..b"));
static_assert(!argumentAccepts(ArgumentKind::Path, "/a/../b"));
static_assert(argumentAccepts(ArgumentKind::Path, R"(C:\wt\contour\r1)"));
static_assert(!argumentAccepts(ArgumentKind::Path, R"(C:\a\..\b)"));

/// One row of a shared corpus in contract/windows-paths.
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

/// Reads a corpus file Contour's host tests read too: one `value<TAB>verdict` row per line, `#` comments.
/// @param file The file's name in contract/windows-paths.
/// @return Its rows, in order.
[[nodiscard]] std::vector<CorpusRow> windowsCorpus(std::string_view file)
{
    auto const text = orchestrator::readFixture(orchestrator::FixtureRoot::Contract,
                                                "windows-paths/" + std::string { file });
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

} // namespace

TEST_CASE("argumentAccepts judges every kind by its rule", "[orchestrator][arguments]")
{
    for (auto const& row: AcceptRows)
    {
        CAPTURE(row.kind, row.value, row.expected);
        auto const verdict = argumentAccepts(row.kind, row.value) ? Verdict::Accepted : Verdict::Refused;
        CHECK(verdict == row.expected);
    }
}

TEST_CASE("the Windows grammar agrees with the host's corpus", "[orchestrator][arguments]")
{
    for (auto const& row: windowsCorpus("names.txt"))
    {
        CAPTURE(row.value);
        CHECK(orchestrator::detail::isPlainWindowsName(row.value) == (row.expected == Verdict::Accepted));
    }
    for (auto const& row: windowsCorpus("paths.txt"))
    {
        CAPTURE(row.value);
        CHECK(orchestrator::detail::isNormalisedAbsolute(row.value) == (row.expected == Verdict::Accepted));
    }
}
