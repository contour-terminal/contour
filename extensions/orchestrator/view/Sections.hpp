// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/tui/Canvas.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <orchestrator/core/Engine.hpp>
#include <orchestrator/view/Band.hpp>
#include <orchestrator/view/Keys.hpp>
#include <orchestrator/view/Theme.hpp>

namespace orchestrator::view
{

/// How long a running agent may go without reporting a status before its row hints at a folder-trust prompt:
/// Claude Code asks once per repository it was never trusted in, in the agent's tab, and reports nothing
/// while it asks. The hint only points at the tab; nothing answers the prompt.
inline constexpr auto TrustPromptHintAfter = std::chrono::seconds { 30 };

/// What a row of the list stands for.
enum class RowKind : uint8_t
{
    Heading,     ///< A section's heading; furniture.
    Spacer,      ///< The blank line before a heading; furniture.
    Placeholder, ///< What an empty section says; furniture.
    Notice,      ///< What the dashboard says when it has nothing to list, such as what to configure.
    PullRequest, ///< One of the user's open pull requests.
    Issue,       ///< An open issue no pull request of the user's claims.
    Agent,       ///< A run that has not finished.
    Finished,    ///< An entry of the Finished list.
    Orphan,      ///< A worktree no run accounts for; read-only.
};

/// What opening a row's details tells the engine.
enum class OpenEffect : uint8_t
{
    None,     ///< Nothing.
    MarkSeen, ///< The finished run was opened: it is no longer new.
};

/// How a detail's value too wide for the panel is drawn.
enum class DetailFit : uint8_t
{
    Wrap, ///< Word-wrapped over as many lines as it takes.
    Cut,  ///< On one line, cut with a `…`: a path or a command line, which a wrap would break where a space
          ///< is, so that a copy of it is no longer what it says.
};

/// One line of a row's details: a label and its value.
struct DetailLine
{
    std::string label = {};          ///< Such as `branch`; empty: the value stands alone.
    std::string value = {};          ///< Sanitised; fitted to the panel when drawn.
    Role role = Role::Text;          ///< How the value is drawn.
    DetailFit fit = DetailFit::Wrap; ///< How it is fitted to the panel.
    [[nodiscard]] bool operator==(DetailLine const&) const = default;
};

/// One row of the list and the details it opens. Every text is sanitised: no control character, one line.
struct Row
{
    RowKind kind = RowKind::Notice;       ///< What it stands for.
    std::string mark = {};                ///< One cell before the text, such as `✓`; empty: a blank.
    Role markRole = Role::Text;           ///< How the mark is drawn.
    std::string prefix = {};              ///< The repository's name, drawn dim; empty with one repository.
    std::string text = {};                ///< The row's text.
    std::string status = {};              ///< Right-aligned after the text, such as `approved`; may be empty.
    Role statusRole = Role::Dim;          ///< How the status is drawn.
    std::string run = {};                 ///< The run it stands for; empty for a pull request or an issue.
    std::string repo = {};                ///< The repository of its item or orphan; empty for a run.
    std::optional<ItemRef> item = {};     ///< The pull request or issue it stands for; absent for a run.
    std::vector<KeyCommand> offers = {};  ///< The run commands it accepts: EndRun, RetryRemoval, Dismiss.
    OpenEffect onOpen = OpenEffect::None; ///< What opening its details tells the engine.
    std::string title = {};               ///< The details' first line.
    std::vector<DetailLine> details = {}; ///< The details' other lines.

    /// @return Whether the row can be selected: it stands for an item, not for the list's furniture.
    [[nodiscard]] bool isSelectable() const noexcept { return kind >= RowKind::PullRequest; }
    /// @return Whether the row is furniture, which a collapsed layout leaves out.
    [[nodiscard]] bool isFurniture() const noexcept { return kind <= RowKind::Placeholder; }
};

/// The rows of the list, in order: per section its heading and its items, the items of every configured
/// repository in the configured order.
/// @param engine The engine.
/// @param nowMonoMs Now, in the monotonic milliseconds the engine's inputs carry: what an agent's age is
///        measured against.
/// @return The rows; the configuration's absence is a notice row that says what to configure.
[[nodiscard]] std::vector<Row> buildRows(Engine const& engine, uint64_t nowMonoMs);

/// @param engine The engine.
/// @param nowMonoMs Now, as buildRows() takes it.
/// @return When an agent's row, as buildRows() builds it, changes next with time alone: its age moves on
///         (`45s` becomes `46s`, `59s` becomes `1m`), or it starts to hint at a trust prompt
///         (TrustPromptHintAfter); std::nullopt when no row changes with time.
[[nodiscard]] std::optional<uint64_t> nextRowChange(Engine const& engine, uint64_t nowMonoMs);

/// The rows a filter and a layout keep.
/// @param rows Every row.
/// @param filter Rows that can be selected and whose text does not hold it, ignoring ASCII case, are left
/// out;
///        empty: none is.
/// @param furniture Whether headings, spacers and placeholders are kept.
/// @return The indices into @p rows of the rows kept, in order.
[[nodiscard]] std::vector<std::size_t> visibleRows(std::span<Row const> rows,
                                                   std::string_view filter,
                                                   Furniture furniture);

/// @param rows Every row.
/// @param shown Indices into @p rows, such as visibleRows() returns.
/// @return How many of the rows @p shown names can be selected.
[[nodiscard]] std::size_t selectableCount(std::span<Row const> rows,
                                          std::span<std::size_t const> shown) noexcept;

/// @param rows Every row.
/// @param shown Indices into @p rows, such as visibleRows() returns.
/// @param index A row among the shown rows that can be selected.
/// @return Its position in @p shown; shown.size() when there is no such row.
[[nodiscard]] std::size_t positionOfSelectable(std::span<Row const> rows,
                                               std::span<std::size_t const> shown,
                                               std::size_t index) noexcept;

/// @param item An item.
/// @return How the dashboard names it: `#2331`, or a Jira key.
[[nodiscard]] std::string itemLabel(ItemRef const& item);

/// Draws a rounded box that fills @p canvas, @p title over its top border.
void drawFrame(core::tui::Canvas& canvas, std::string_view title);

/// @return The part of @p canvas inside a box's border.
[[nodiscard]] core::tui::Canvas insideFrame(core::tui::Canvas const& canvas);

/// What separates two hints on a hint line.
inline constexpr auto HintSeparator = std::string_view { " · " };

/// A hint line: the hints joined by HintSeparator, as many whole ones as fit; a hint that does not fit is
/// left out with every one after it, so the line is never cut inside a hint.
/// @param hints The hints, most important first.
/// @param columns The cells the line may take.
/// @return The line.
[[nodiscard]] std::string joinHints(std::span<std::string_view const> hints, int columns);

/// @param text Text.
/// @return How many cells it takes.
[[nodiscard]] int columnsOf(std::string_view text);

/// Text safe to draw, cut to a width. Never wrapped, and never half a wide character: a cut ends in `…`.
/// @param text Untrusted UTF-8.
/// @param columns The most cells the result may take.
/// @return The text sanitised (see displayText()), and when it is wider than @p columns, its longest prefix
///         of whole grapheme clusters that leaves a cell for the `…` that follows it.
[[nodiscard]] std::string cut(std::string_view text, int columns);

/// Text safe to draw, word-wrapped. Lines break at spaces; a word wider than a line is broken between
/// grapheme clusters, never inside a wide character.
/// @param text Untrusted UTF-8, sanitised as cut() does.
/// @param firstColumns The cells the first line may take.
/// @param nextColumns The cells each further line may take, such as @p firstColumns less an indent.
/// @return The lines, in order; empty when @p text is or @p firstColumns is not positive.
[[nodiscard]] std::vector<std::string> wrap(std::string_view text, int firstColumns, int nextColumns);

} // namespace orchestrator::view
