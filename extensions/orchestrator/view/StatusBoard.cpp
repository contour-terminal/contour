// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/view/StatusBoard.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include <orchestrator/core/Run.hpp>
#include <orchestrator/core/Sanitize.hpp>
#include <orchestrator/view/Sections.hpp>
#include <orchestrator/view/Theme.hpp>

using namespace std::string_view_literals;

namespace orchestrator::view
{

namespace
{
    constexpr auto MinuteMs = uint64_t { 60'000 };
    constexpr auto HourMs = 60 * MinuteMs;
    constexpr auto DayMs = 24 * HourMs;

    /// One way of saying an age: the ages below @ref below are counted in @ref unit.
    struct AgeUnit
    {
        uint64_t below = 0;           ///< The first age it no longer says; 0: every age.
        uint64_t unit = 0;            ///< What it counts in, in milliseconds.
        std::string_view suffix = {}; ///< After the count; empty: the age is `just now`, uncounted.
    };

    /// The ways an age is said, youngest first: the first whose @ref AgeUnit::below lies past the age says
    /// it.
    constexpr auto AgeUnits = std::array {
        AgeUnit { .below = MinuteMs, .unit = MinuteMs, .suffix = ""sv },
        AgeUnit { .below = HourMs, .unit = MinuteMs, .suffix = " min ago"sv },
        AgeUnit { .below = 48 * HourMs, .unit = HourMs, .suffix = " h ago"sv },
        AgeUnit { .below = 0, .unit = DayMs, .suffix = " days ago"sv },
    };

    /// What a board that is not shown says instead, by its state and, when Unavailable, its reason.
    struct BoardStateRow
    {
        BoardState state = BoardState::Unread;   ///< The board's state.
        std::optional<FileReadError> error = {}; ///< Its reason, for an Unavailable board; none: any.
        std::string_view text = {};              ///< What it says.
    };

    /// What a board says once its run is gone: dismissed, here or in another tab.
    constexpr auto DismissedText = "This run was dismissed."sv;

    /// What a finished run's board says once its worktree was removed: the run ended, not the prompt failed.
    constexpr auto RemovedText = "The board was removed when the run ended."sv;

    /// What a finished run's board says when its worktree holds none: the agent can no longer write one.
    constexpr auto NeverWrittenText = "This run wrote no STATUS.md."sv;

    /// What a board says while the configuration is refused: the engine reads nothing then.
    constexpr auto UnconfiguredText =
        "STATUS.md is not read while the configuration is refused; fix it to see the board."sv;

    /// The first row that matches a board says it.
    constexpr auto BoardStateRows = std::array {
        BoardStateRow { .state = BoardState::Unread, .error = std::nullopt, .text = "reading STATUS.md…"sv },
        BoardStateRow { .state = BoardState::Absent,
                        .error = std::nullopt,
                        .text =
                            "No STATUS.md to show yet. Agents write one when their prompt asks for it."sv },
        BoardStateRow {
            .state = BoardState::Gone, .error = std::nullopt, .text = "The board went with its worktree."sv },
        BoardStateRow { .state = BoardState::Unavailable,
                        .error = FileReadError::TooLarge,
                        .text = "STATUS.md is larger than 64 KiB."sv },
        BoardStateRow { .state = BoardState::Unavailable,
                        .error = FileReadError::Escapes,
                        .text = "STATUS.md leads outside the worktree."sv },
        BoardStateRow { .state = BoardState::Unavailable,
                        .error = std::nullopt,
                        .text = "STATUS.md could not be read."sv },
    };

    /// @return Now, in the monotonic milliseconds of @p clock.
    [[nodiscard]] uint64_t monoMsOf(core::IClock const& clock) noexcept
    {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(clock.now().time_since_epoch()).count());
    }
} // namespace

BoardAge boardAgeOf(uint64_t ageMs)
{
    for (auto const& unit: AgeUnits)
    {
        if (unit.below != 0 && ageMs >= unit.below)
            continue;
        auto const count = ageMs / unit.unit;
        auto const text =
            unit.suffix.empty() ? std::string { "just now" } : std::format("{}{}", count, unit.suffix);
        return BoardAge { .text = text, .changesInMs = ((count + 1) * unit.unit) - ageMs };
    }
    return BoardAge {};
}

std::optional<std::string_view> boardStateText(Board const* board) noexcept
{
    auto const state = board != nullptr ? board->state : BoardState::Unread;
    if (state == BoardState::Read)
        return std::nullopt;
    // A loop rather than std::ranges::find_if, for the reason Sanitize.cpp's dispositionOf() gives.
    for (auto const& row: BoardStateRows)
        if (row.state == state && (!row.error || (board != nullptr && row.error == board->error)))
            return row.text;
    return std::nullopt;
}

std::optional<std::string_view> boardScreenText(Engine const& engine, std::string_view run) noexcept
{
    // The engine reads no board of a run it no longer knows, nor any while its configuration is refused.
    auto const runs = engine.runs();
    auto const found = std::ranges::find(runs, run, &Run::id);
    if (found == runs.end())
        return DismissedText;
    if (engine.config() == nullptr)
        return UnconfiguredText;
    auto const* const board = engine.board(run);
    // A finished run's agent writes no more: its board is the one its worktree kept, or none.
    if (!isUnfinished(*found))
    {
        if (!keepsWorktree(*found) || (board != nullptr && board->state == BoardState::Gone))
            return RemovedText;
        if (board != nullptr && board->state == BoardState::Absent)
            return NeverWrittenText;
    }
    return boardStateText(board);
}

namespace
{
    // A board's contents as drawn lines: sanitised (displayLines()), each line word-wrapped to the
    // columns with its indentation as the continuation's, at most MaxBoardLines in all, the last a `…` when
    // more were cut.
    std::vector<std::string> boardLines(std::string_view bytes, int columns)
    {
        auto lines = std::vector<std::string> {};
        if (columns <= 0)
            return lines;
        for (auto const& line: displayLines(bytes, MaxBoardLines, MaxBoardLineChars))
        {
            auto const text =
                std::string_view { line }.substr(std::min(line.find_first_not_of(' '), line.size()));
            if (text.empty())
            {
                lines.emplace_back();
                continue;
            }
            // An indentation the line cannot keep beside some text is narrowed to half of it.
            auto const indent = std::min(static_cast<int>(line.size() - text.size()), columns / 2);
            for (auto& piece: wrap(text, columns - indent, columns - indent))
                lines.push_back(std::string(static_cast<std::size_t>(indent), ' ') + std::move(piece));
        }
        // A cut line, or a wrapped one, never draws past the cap.
        if (lines.size() > MaxBoardLines)
        {
            lines.resize(MaxBoardLines);
            lines.back() = "…";
        }
        return lines;
    }
} // namespace

StatusBoardPanel::StatusBoardPanel(Engine const& engine,
                                   Interaction const& interaction,
                                   core::IClock const& clock,
                                   BoardPanelState& state):
    _engine { engine }, _interaction { interaction }, _clock { clock }, _state { state }
{
}

std::vector<std::string> const& StatusBoardPanel::linesFor(std::string_view bytes, int columns)
{
    if (columns != _wrappedColumns || bytes != _wrappedBytes)
    {
        _wrapped = boardLines(bytes, columns);
        _wrappedBytes = bytes;
        _wrappedColumns = columns;
    }
    return _wrapped;
}

void StatusBoardPanel::render(core::tui::Canvas& canvas)
{
    // The Narrow band shows the board alone, without the summary line that hides the cursor elsewhere.
    canvas.hideCursor();
    auto const& theme = canvas.theme();
    drawFrame(canvas, std::format("STATUS.md · {}", _interaction.subject));
    auto inside = insideFrame(canvas);
    auto const width = inside.width() - 2;
    auto const height = std::max(0, inside.height());
    auto const* const shown = _engine.board(_interaction.subject);
    _state = BoardPanelState {};

    if (auto const text = boardScreenText(_engine, _interaction.subject); text || shown == nullptr)
    {
        auto line = 0;
        for (auto const& piece: wrap(text.value_or(""), width, width) | std::views::take(height))
            inside.putString(line++, 1, piece, styleOf(theme, Role::Dim));
        return;
    }

    auto const& board = *shown;
    // The age is the wall time now, carried on from the read by the monotonic clock, less the file's.
    auto const nowMonoMs = monoMsOf(_clock);
    if (board.modifiedWallMs != 0 && board.readWallMs != 0)
    {
        auto const nowWallMs =
            board.readWallMs + (nowMonoMs > board.readMonoMs ? nowMonoMs - board.readMonoMs : 0);
        auto const age = boardAgeOf(nowWallMs > board.modifiedWallMs ? nowWallMs - board.modifiedWallMs : 0);
        inside.putString(0, 1, cut(std::format("updated {}", age.text), width), styleOf(theme, Role::Dim));
        _state.ageChangeMs = nowMonoMs + age.changesInMs;
    }
    else
        inside.putString(0, 1, cut("updated at an unknown time", width), styleOf(theme, Role::Dim));

    auto const& lines = linesFor(board.bytes, width);
    auto const rows = static_cast<std::size_t>(std::max(0, height - 1));
    _state.lines = lines.size();
    _state.rows = rows;
    // The keys keep the scroll within the board as last drawn; one that shrank since is shown from its end.
    auto const top = std::min(_interaction.scroll, lines.size() > rows ? lines.size() - rows : 0);
    if (lines.empty())
    {
        inside.putString(1, 1, cut("STATUS.md is empty.", width), styleOf(theme, Role::Dim));
        return;
    }
    auto line = 1;
    for (auto const& text: lines | std::views::drop(top) | std::views::take(rows))
        inside.putString(line++, 1, text, styleOf(theme, Role::Text));
}

} // namespace orchestrator::view
