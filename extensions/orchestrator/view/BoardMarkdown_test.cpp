// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <vector>

#include <orchestrator/view/BoardMarkdown.hpp>

using orchestrator::view::BoardFit;
using orchestrator::view::BoardLine;
using orchestrator::view::renderBoardMarkdown;
using orchestrator::view::Role;

namespace
{

[[nodiscard]] std::vector<BoardLine> rendered(std::vector<std::string> const& lines)
{
    return renderBoardMarkdown(lines);
}

} // namespace

TEST_CASE("BoardMarkdown: headings, lists, task boxes, quotes and rules", "[orchestrator][view][board]")
{
    CHECK(rendered({ "# Fix CI", "## Done", "#hashtag" })
          == std::vector { BoardLine { .text = "Fix CI", .role = Role::Accent },
                           BoardLine { .text = "Done", .role = Role::Accent },
                           // No space after the hashes: no heading.
                           BoardLine { .text = "#hashtag", .role = Role::Text } });
    CHECK(rendered({ "- one", "  * nested", "- [ ] todo", "- [x] done", "> quoted", "---" })
          == std::vector {
              BoardLine { .text = "• one", .role = Role::Text },
              BoardLine { .text = "  • nested", .role = Role::Text },
              BoardLine { .text = "☐ todo", .role = Role::Text },
              BoardLine { .text = "☑ done", .role = Role::Passing },
              BoardLine { .text = "│ quoted", .role = Role::Note },
              BoardLine { .text = std::string { "────────────────────────" }, .role = Role::Dim } });
}

TEST_CASE("BoardMarkdown: inline markers and links", "[orchestrator][view][board]")
{
    CHECK(rendered({ "Run **ctest** in `out/` -- see [the PR](https://x/1)." })
          == std::vector { BoardLine { .text = "Run ctest in out/ -- see the PR.", .role = Role::Text } });
    // An unclosed link stays as it is.
    CHECK(rendered({ "[not a link" })
          == std::vector { BoardLine { .text = "[not a link", .role = Role::Text } });
}

TEST_CASE("BoardMarkdown: a table is aligned, its separator dropped", "[orchestrator][view][board]")
{
    CHECK(
        rendered({ "| Phase | State |", "|-------|:-----:|", "| build | done |", "| test | running |" })
        == std::vector { BoardLine { .text = "Phase │ State", .role = Role::Accent, .fit = BoardFit::Cut },
                         BoardLine { .text = "build │ done", .role = Role::Text, .fit = BoardFit::Cut },
                         BoardLine { .text = "test  │ running", .role = Role::Text, .fit = BoardFit::Cut } });
}

TEST_CASE("BoardMarkdown: a fenced block is dim and keeps its markers", "[orchestrator][view][board]")
{
    CHECK(rendered({ "```sh", "- not a list", "**x**", "```", "- a list" })
          == std::vector { BoardLine { .text = "- not a list", .role = Role::Dim, .fit = BoardFit::Cut },
                           BoardLine { .text = "**x**", .role = Role::Dim, .fit = BoardFit::Cut },
                           BoardLine { .text = "• a list", .role = Role::Text } });
}
