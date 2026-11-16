// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/tui/TerminalOutput.hpp>

#include <cstdint>
#include <expected>
#include <utility>

namespace core::tui
{

/// @brief Why a terminal query has no answer.
///
/// Returned in place of a value that used to stand in for "nothing" -- `(0, 0)` for a cursor
/// position, which is not even a legal one. A query nobody sent and a terminal that stayed silent
/// are different facts, and a caller deciding what to do next needs to know which it has.
enum class QueryUnanswered : std::uint8_t
{
    NotAsked, ///< No query was sent: this terminal has no input to read a reply from (mock output).
    NoReply,  ///< The query was sent and nothing answered before the timeout.
};

/// @brief What a @c Screen draws on: a size, the output its frames are written to, and the one
/// question the inline viewport asks.
///
/// @c Terminal is the implementation on a real terminal. The interface is what keeps @c Screen
/// free of one: nothing here reads a descriptor, installs a signal handler or waits, so a
/// @c Screen and everything it renders build without the platform layer or the event loop
/// (core::tui_core, docs/design/tui-headless.md).
class ScreenSurface
{
  public:
    ScreenSurface() = default;
    virtual ~ScreenSurface() = default;

    ScreenSurface(ScreenSurface const&) = delete;
    ScreenSurface& operator=(ScreenSurface const&) = delete;
    ScreenSurface(ScreenSurface&&) = delete;
    ScreenSurface& operator=(ScreenSurface&&) = delete;

    /// @brief Returns the surface's width in columns.
    [[nodiscard]] virtual auto columns() const noexcept -> int = 0;

    /// @brief Returns the surface's height in rows.
    [[nodiscard]] virtual auto rows() const noexcept -> int = 0;

    /// @brief Returns the output every byte of a frame is written to.
    [[nodiscard]] virtual auto output() noexcept -> TerminalOutput& = 0;

    /// @brief Asks where the cursor is. Only the inline viewport asks, to learn the row it starts on.
    /// @return Pair of (row, column), both 1-based, or why there is none.
    [[nodiscard]] virtual auto queryCursorPosition()
        -> std::expected<std::pair<int, int>, QueryUnanswered> = 0;
};

} // namespace core::tui
