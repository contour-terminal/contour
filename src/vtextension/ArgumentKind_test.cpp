// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <vtextension/ArgumentKind.hpp>

using namespace std::string_literals;
using namespace std::string_view_literals;
using vtextension::argumentAccepts;
using vtextension::ArgumentKind;
using vtextension::argumentKindOf;

static_assert(argumentAccepts(ArgumentKind::Number, "1"));

namespace
{

enum class Verdict : uint8_t
{
    Refused,
    Accepted,
};

struct Row
{
    ArgumentKind kind;
    std::string value;
    Verdict expected;
};

/// An absolute path of exactly @p bytes bytes: `/` and then `a`s.
[[nodiscard]] std::string pathOf(std::size_t bytes)
{
    return "/" + std::string(bytes - 1, 'a');
}

} // namespace

TEST_CASE("argumentAccepts", "[vtextension][manifest]")
{
    using enum ArgumentKind;
    using enum Verdict;
    auto const uuid = "550e8400-e29b-41d4-a716-446655440000"s;
    auto const rows = std::vector<Row> {
        { Number, "2334", Accepted },
        { Number, "", Refused },
        { Number, "-1", Refused },
        { Number, "12345678901", Refused },
        { Number, "12a", Refused },

        { Name, "origin", Accepted },
        { Name, "claude-opus-5-5", Accepted },
        { Name, "ext::sh", Refused },
        { Name, "-x", Refused },
        { Name, ".hidden", Refused },
        { Name, "a/b", Refused },
        { Name, "https://evil", Refused },
        { Name, std::string(64, 'n'), Accepted },
        { Name, std::string(65, 'n'), Refused },

        { Ref, "origin/master", Accepted },
        { Ref, "agent/pr-1-20261113-141207", Accepted },
        { Ref, "user@branch", Accepted },
        { Ref, "fix#12", Accepted },
        { Ref, "c++", Accepted },
        { Ref, "a..b", Refused },
        { Ref, "a//b", Refused },
        { Ref, "-a", Refused },
        { Ref, "/a", Refused },
        { Ref, "a b", Refused },
        { Ref, "a:b", Refused },
        { Ref, "a~1", Refused },
        { Ref, "a@{1}", Refused },
        { Ref, std::string(200, 'r'), Accepted },
        { Ref, std::string(201, 'r'), Refused },

        { Path, "/home/u/worktrees/c/pr-1", Accepted },
        { Path, pathOf(vtextension::MaxPathArgumentBytes), Accepted },
        { Path, "rel/x", Refused },
        { Path, "/a/../b", Refused },
        { Path, "/a/./b", Refused },
        { Path, "/a//b", Refused },
        { Path, "/a\0b"s, Refused },
        { Path, pathOf(vtextension::MaxPathArgumentBytes + 1), Refused },

        { Uuid, uuid, Accepted },
        { Uuid, "550E8400-E29B-41D4-A716-446655440000", Refused },
        { Uuid, "550e8400", Refused },
        { Uuid, "550e8400-e29b-41d4-a716-44665544000g", Refused },
        { Uuid, "550e8400e29b-41d4-a716-4466554400000", Refused },

        { Text, "assignee:@me", Accepted },
        { Text, "Fix the bug.\nThen push.", Accepted },
        { Text, "-label:x", Refused },
        { Text, "a\0b"s, Refused },
        { Text, std::string(vtextension::MaxTextArgumentBytes, 't'), Accepted },
        { Text, std::string(vtextension::MaxTextArgumentBytes + 1, 't'), Refused },
    };
    for (auto const& row: rows)
    {
        CAPTURE(row.kind, row.value);
        CHECK(argumentAccepts(row.kind, row.value) == (row.expected == Accepted));
    }
}

TEST_CASE("argumentKindOf", "[vtextension][manifest]")
{
    CHECK(argumentKindOf("number") == ArgumentKind::Number);
    CHECK(argumentKindOf("uuid") == ArgumentKind::Uuid);
    CHECK(argumentKindOf("name") == ArgumentKind::Name);
    CHECK(argumentKindOf("ref") == ArgumentKind::Ref);
    CHECK(argumentKindOf("text") == ArgumentKind::Text);
    CHECK(argumentKindOf("path:worktrees") == ArgumentKind::Path);
    CHECK_FALSE(argumentKindOf("path:"));
    CHECK_FALSE(argumentKindOf("path"));
    CHECK_FALSE(argumentKindOf("url"));
    CHECK_FALSE(argumentKindOf("Number"));
    CHECK_FALSE(argumentKindOf(""));
}
