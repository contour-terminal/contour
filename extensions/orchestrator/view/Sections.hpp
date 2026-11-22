// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/tui/Canvas.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <orchestrator/core/Engine.hpp>
#include <orchestrator/core/QuickReplies.hpp>
#include <orchestrator/view/Band.hpp>
#include <orchestrator/view/Fleet.hpp>
#include <orchestrator/view/Keys.hpp>
#include <orchestrator/view/Theme.hpp>

namespace orchestrator::view
{

/// How long a running agent may go without reporting a status before its row hints at a folder-trust prompt:
/// Claude Code asks once per repository it was never trusted in, in the agent's tab, and reports nothing
/// while it asks. The hint only points at the tab; nothing answers the prompt.
inline constexpr auto TrustPromptHintAfter = std::chrono::seconds { 30 };

/// What a row of the Fleet stands for.
enum class RowKind : uint8_t
{
    Notice = 0, ///< What the dashboard says when it has nothing to list, such as what to configure.
    Heading,    ///< A group's heading; selectable while the group hides some of its runs.
    Agent,      ///< A run that has not finished.
    Finished,   ///< A run of the Finished group.
    Orphan,     ///< A worktree no run accounts for; read-only.
    NewAgent,   ///< `+ new agent`: Enter opens the launcher.
};

/// How a detail's value too wide for the panel is drawn.
enum class DetailFit : uint8_t
{
    Wrap,  ///< Word-wrapped over as many lines as it takes.
    Cut,   ///< On one line, cut with a `…`: a path or a command line, which a wrap would break where a space
           ///< is, so that a copy of it is no longer what it says.
    Whole, ///< On one line and whole, or not at all: a command line, which a cut would shorten and a break
           ///< would split inside an argument a copy then carries. One that does not fit says
           ///< WidenToSeeText in its place.
    Aligned, ///< On one line, cut with a `…`, its runs of spaces kept: a board's table row or code, sanitised
             ///< already (displayLines()), whose alignment a second sanitising would collapse.
};

/// One line of a row's details: a label and its value.
struct DetailLine
{
    std::string label = {};          ///< Such as `branch`; empty: the value stands alone.
    std::string value = {};          ///< Sanitised; fitted to the panel when drawn.
    Role role = Role::Text;          ///< How the value is drawn.
    DetailFit fit = DetailFit::Wrap; ///< How it is fitted to the panel.
    std::string link = {};           ///< The https URL its value is drawn as a hyperlink to; empty: none.
    [[nodiscard]] bool operator==(DetailLine const&) const = default;
};

/// Whether a group of the Fleet shows all of its runs.
enum class Fold : uint8_t
{
    Open = 0,
    Folded,
};

/// One row of the Fleet and the details it opens. Every text is sanitised: no control character, one line.
struct Row
{
    RowKind kind = RowKind::Notice; ///< What it stands for.
    std::string mark = {};          ///< One cell before the text, such as `✓`; empty: a blank.
    Role markRole = Role::Text;     ///< How the mark is drawn.
    std::string prefix = {};        ///< The repository's name, drawn dim; empty with one repository.
    std::string text = {};          ///< The row's first line: the run and its task.
    std::string status = {};        ///< Right-aligned after the text, such as `asking 2m`; may be empty.
    Role statusRole = Role::Dim;    ///< How the status is drawn.
    std::string context = {};       ///< Its second line: what the agent is on, or how the run ended.
    Role contextRole = Role::Dim;   ///< How the second line is drawn.
    std::optional<BoardProgress> progress = {}; ///< What its board's task boxes say, where a board was read.
    std::string run = {};  ///< The run it stands for; empty for a heading, an orphan or a notice.
    std::string name = {}; ///< How the run is named, such as `pr-2375` or a topic's text.
    std::string repo = {}; ///< The repository of its orphan; empty for a run.
    std::vector<KeyCommand> offers = {}; ///< The commands it accepts beyond moving: the Focus tabs, EndRun,
                                         ///< RetryRemoval, Dismiss, Reveal, OpenLink.
    std::string group = {}; ///< The group it heads or belongs to, such as `fleet/finished`; empty:
                            ///< none. A group's key is what the engine keeps folded.
    Fold fold = Fold::Open; ///< A heading: whether its group hides some runs; a run: whether it is
                            ///< hidden, which it is not while a filter is typed.
    std::optional<FleetGroup> attention = {}; ///< The group a run is listed in; none for any other row.
    std::string state = {};             ///< The word of its run's look (runLookOf()): a tab the user chose
                                        ///< sticks until it changes.
    RowState rowState = RowState::None; ///< What it is, as far as the actions it offers go (actionsFor()).
    std::string title = {};             ///< The Focus header's second line begins with it: the item's title.
    std::vector<DetailLine> details = {}; ///< The Details tab's lines.
    News news = News::None; ///< Whether its run changed since the user last looked at it; set by the
                            ///< dashboard before each draw.
    std::vector<QuickReply> replies = {}; ///< An asking run's chips: the configured quick replies; empty for
                                          ///< any other row.

    /// @return Whether the row can be selected: it stands for a run, a worktree or `+ new agent`; a heading
    /// only while its group hides some of its runs.
    [[nodiscard]] bool isSelectable() const noexcept
    {
        return kind > RowKind::Heading || (kind == RowKind::Heading && fold == Fold::Folded);
    }
};

/// The rows of the Fleet, in order: per group (fleetGroups()) its heading and its runs, longest waiting
/// first, the Finished newest first; the worktrees no run accounts for; and `+ new agent`.
/// @param engine The engine.
/// @param nowMonoMs Now, in the monotonic milliseconds the engine's inputs carry: what an agent's age is
///        measured against.
/// @return The rows; the configuration's absence is a notice row that says what to configure.
[[nodiscard]] std::vector<Row> buildRows(Engine const& engine, uint64_t nowMonoMs);

/// @param count A count, such as of tokens.
/// @return It shortened: such as `152`, `42.3k` or `12.9M`.
[[nodiscard]] std::string countText(uint64_t count);

/// @param engine The engine.
/// @param nowMonoMs Now, as buildRows() takes it.
/// @return When an agent's row, as buildRows() builds it, changes next with time alone: its age moves on
///         (`45s` becomes `46s`, `59s` becomes `1m`), or it starts to hint at a trust prompt
///         (TrustPromptHintAfter); std::nullopt when no row changes with time.
[[nodiscard]] std::optional<uint64_t> nextRowChange(Engine const& engine, uint64_t nowMonoMs);

/// The rows a filter keeps.
/// @param rows Every row.
/// @param filter Rows that can be selected and whose text does not hold it, ignoring ASCII case, are left
///        out, and a folded group shows every run that holds it; empty: none is, and a folded group's hidden
///        runs are left out.
/// @return The indices into @p rows of the rows kept, in order.
[[nodiscard]] std::vector<std::size_t> visibleRows(std::span<Row const> rows, std::string_view filter);

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

/// How a value is shown: a mark, its role, and a word.
struct Look
{
    std::string_view mark = {}; ///< One cell, or empty.
    Role role = Role::Text;     ///< How the mark and the word are drawn.
    std::string_view word = {}; ///< What the row's status or a detail says.
};

/// A change request's checks, indexed by CiState.
inline constexpr auto CiLooks = std::array {
    Look { .mark = std::string_view {}, .role = Role::Dim, .word = "no checks" },
    Look { .mark = "●", .role = Role::Warning, .word = "running" },
    Look { .mark = "✓", .role = Role::Passing, .word = "passing" },
    Look { .mark = "✗", .role = Role::Failing, .word = "failing" },
};

/// A change request's review, indexed by ReviewState.
inline constexpr auto ReviewLooks = std::array {
    Look { .mark = std::string_view {}, .role = Role::Dim, .word = std::string_view {} },
    Look { .mark = "?", .role = Role::Dim, .word = "review required" },
    Look { .mark = "✓", .role = Role::Passing, .word = "approved" },
    Look { .mark = "△", .role = Role::Warning, .word = "changes requested" },
};

static_assert(CiLooks.size() == std::to_underlying(CiState::Failing) + 1U);
static_assert(ReviewLooks.size() == std::to_underlying(ReviewState::ChangesRequested) + 1U);

/// The entry of @p table that @p value indexes.
template <typename Entry, std::size_t N, typename Enum>
[[nodiscard]] constexpr Entry const& entryOf(std::array<Entry, N> const& table, Enum value) noexcept
{
    auto const index = static_cast<std::size_t>(std::to_underlying(value));
    return table[index < N ? index : 0];
}

/// @param c A character.
/// @return @p c with an ASCII capital lower-cased.
[[nodiscard]] constexpr char asciiLower(char c) noexcept
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

/// @return Whether @p haystack holds @p needle, ignoring ASCII case.
[[nodiscard]] bool containsIgnoringCase(std::string_view haystack, std::string_view needle) noexcept;

/// @return Whether @p a and @p b are equal, ignoring ASCII case.
[[nodiscard]] bool equalsIgnoringCase(std::string_view a, std::string_view b) noexcept;

/// @param item An item.
/// @return How the dashboard names it: `#2331`, or a Jira key.
[[nodiscard]] std::string itemLabel(ItemRef const& item);

/// Draws a rounded box that fills @p canvas, @p title over its top border.
/// @param canvas Where.
/// @param title Its title; empty: none.
/// @param border How the border is drawn: Role::Focus for the place that has the keys.
void drawFrame(core::tui::Canvas& canvas, std::string_view title, Role border = Role::Border);

/// @return The part of @p canvas inside a box's border.
[[nodiscard]] core::tui::Canvas insideFrame(core::tui::Canvas const& canvas);

/// Draws @p text, and makes every whole `https://` or `http://` URL in it a hyperlink (OSC 8): a URL cut
/// short is drawn, never linked, since its link would lead elsewhere.
/// @param canvas Where.
/// @param row The row.
/// @param column The first cell.
/// @param text Sanitised text that fits.
/// @param style How it is drawn.
/// @return The cells it took.
int putLinked(
    core::tui::Canvas& canvas, int row, int column, std::string_view text, core::tui::Style const& style);

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

/// cut() for text sanitised already, such as a board's line (displayLines()): its runs of spaces are kept.
/// @param sanitised Sanitised UTF-8: no control character.
/// @param columns The most cells the result may take.
/// @return @p sanitised, or its longest prefix of whole grapheme clusters that leaves a cell for a `…`.
[[nodiscard]] std::string cutSanitised(std::string_view sanitised, int columns);

/// Text safe to draw, word-wrapped. Lines break at spaces; a word wider than a line is broken between
/// grapheme clusters, never inside a wide character.
/// @param text Untrusted UTF-8, sanitised as cut() does.
/// @param firstColumns The cells the first line may take.
/// @param nextColumns The cells each further line may take, such as @p firstColumns less an indent.
/// @return The lines, in order; empty when @p text is or @p firstColumns is not positive.
[[nodiscard]] std::vector<std::string> wrap(std::string_view text, int firstColumns, int nextColumns);

/// What a DetailFit::Whole line too wide for its cells says instead.
inline constexpr std::string_view WidenToSeeText = "widen the window to see the command";

/// @param text Untrusted UTF-8.
/// @param fit How it is fitted: cut(), wrap(), or whole on one line (WidenToSeeText, cut, where it does not
///            fit).
/// @param firstColumns The cells the first line may take.
/// @param nextColumns The cells each further line may take.
/// @return Its lines, as @p fit fits them.
[[nodiscard]] std::vector<std::string> fitted(std::string_view text,
                                              DetailFit fit,
                                              int firstColumns,
                                              int nextColumns);

/// One line that displayLines() returned, word-wrapped to @p columns with its indentation as the
/// continuation's, narrowed to half the width where it would leave less.
/// @param line A sanitised line, its indentation spaces.
/// @param columns The cells a line may take.
/// @return Its pieces, in order; one empty one for a line with no text.
[[nodiscard]] std::vector<std::string> wrapIndented(std::string_view line, int columns);

/// Untrusted multi-line text as drawn lines: sanitised (displayLines()), each line word-wrapped to
/// @p columns with its indentation as the continuation's (narrowed to half the width where it would leave
/// less), at most @p maxLines in all, the last a `…` when more were cut.
/// @param text The text, untrusted, as read.
/// @param columns The cells a line may take.
/// @param maxLines The most lines returned.
/// @param maxLineChars The most characters of a line of @p text after its indentation; a longer one is cut
///        with a `…` before it is wrapped.
/// @return The lines, in order.
[[nodiscard]] std::vector<std::string> indentedLines(std::string_view text,
                                                     int columns,
                                                     std::size_t maxLines,
                                                     std::size_t maxLineChars);

} // namespace orchestrator::view
