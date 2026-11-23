// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <orchestrator/core/ArgumentKinds.hpp>

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
