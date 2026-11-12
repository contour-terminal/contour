// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What the close confirmation says for each kind of node a sidebar menu closes: its title, the body naming
/// what ends, and the label of the destructive button. One table row per kind, so the dialog is one
/// component with no per-kind branches, and a fourth kind is a fourth row. Pure and Qt-free.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <ranges>
#include <string>
#include <string_view>

namespace contour::window
{

/// The kind of node a close confirmation is about.
enum class CloseKind : uint8_t
{
    Workspace = 0, ///< A workspace, with every tab and pane in it.
    Tab,           ///< A tab, with every pane in it.
    Pane,          ///< One pane.
};

/// Number of @c CloseKind enumerators, derived from the last one.
inline constexpr size_t CloseKindCount = static_cast<size_t>(CloseKind::Pane) + 1;

/// What is about to end, as the dialog names it.
struct CloseSubject
{
    std::string label;   ///< The node's name; empty when it has none.
    size_t tabs = 0;     ///< How many tabs end with it.
    size_t panes = 0;    ///< How many panes end with it.
    std::string program; ///< The running program or command, where known; empty otherwise.
};

/// One kind's wording. The format strings take, by position: {0} the label, {1} the tabs as words (`2 tabs`),
/// {2} the panes as words (`3 panes`), {3} the program.
struct CloseDialogRow
{
    CloseKind kind = CloseKind::Workspace; ///< The kind the row describes.
    std::string_view title;                ///< The heading when the node has a name.
    std::string_view untitledTitle;        ///< The heading when it has none.
    std::string_view ends;                 ///< What ends with it.
    std::string_view running;              ///< Said after it when a program is known to run.
    std::string_view destructive;          ///< The destructive button.
};

/// The rows, in CloseKind order.
inline constexpr auto CloseDialogRows = std::array {
    CloseDialogRow { .kind = CloseKind::Workspace,
                     .title = "Close the workspace “{0}”?",
                     .untitledTitle = "Close this workspace?",
                     .ends = "This ends {1} and {2}.",
                     .running = "“{3}” is still running.",
                     .destructive = "Close Workspace" },
    CloseDialogRow { .kind = CloseKind::Tab,
                     .title = "Close the tab “{0}”?",
                     .untitledTitle = "Close this tab?",
                     .ends = "This ends the tab and its {2}.",
                     .running = "“{3}” is still running.",
                     .destructive = "Close Tab" },
    CloseDialogRow { .kind = CloseKind::Pane,
                     .title = "Close the pane “{0}”?",
                     .untitledTitle = "Close this pane?",
                     .ends = "This ends the pane and its shell.",
                     .running = "“{3}” is still running.",
                     .destructive = "Close Pane" },
};

static_assert(std::ranges::all_of(std::views::iota(size_t { 0 }, CloseDialogRows.size()),
                                  [](size_t index) {
                                      return CloseDialogRows[index].kind == static_cast<CloseKind>(index);
                                  }),
              "CloseDialogRows lists every CloseKind once, in enumerator order");

/// What the dialog shows.
struct CloseDialogText
{
    std::string heading;    ///< The title.
    std::string message;    ///< What ends, and what runs.
    std::string acceptText; ///< The destructive button's label.
};

namespace detail
{
    /// @return `1 tab`, `2 tabs`.
    [[nodiscard]] inline std::string countOf(size_t count, std::string_view noun)
    {
        return std::format("{} {}{}", count, noun, count == 1 ? "" : "s");
    }
} // namespace detail

/// @param kind    The kind of node.
/// @param subject What is about to end.
/// @return The dialog's wording for it.
[[nodiscard]] inline CloseDialogText closeDialogText(CloseKind kind, CloseSubject const& subject)
{
    auto const& row = CloseDialogRows[static_cast<size_t>(kind)];
    auto const tabs = detail::countOf(subject.tabs, "tab");
    auto const panes = detail::countOf(subject.panes, "pane");
    auto const fill = [&](std::string_view pattern) {
        return std::vformat(pattern, std::make_format_args(subject.label, tabs, panes, subject.program));
    };
    auto message = fill(row.ends);
    if (!subject.program.empty())
        message += ' ' + fill(row.running);
    return { .heading = fill(subject.label.empty() ? row.untitledTitle : row.title),
             .message = std::move(message),
             .acceptText = std::string { row.destructive } };
}

} // namespace contour::window
