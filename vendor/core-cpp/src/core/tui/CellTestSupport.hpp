// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Test-only: reaching a cell of a @c Buffer from a Catch2 case. @c Buffer::cellAt() returns null
/// outside the buffer, and a test that dereferences it unchecked turns a wrong coordinate into a
/// crash of the whole binary rather than one failed case. @c requireCell() fails the case instead.

#include <core/tui/Buffer.hpp>
#include <core/tui/Cell.hpp>
#include <core/tui/Rect.hpp>

#include <catch2/catch_test_macros.hpp>

namespace core::tui::test
{

/// @brief The cell at @p position, failing the running case first if it is outside @p buffer.
/// @param buffer The buffer to read.
/// @param position The cell's column (x) and row (y), 0-based.
/// @return The cell; the case has already ended when there is none.
[[nodiscard]] inline auto requireCell(Buffer& buffer, Point position) -> Cell&
{
    auto* const cell = buffer.cellAt(position);
    REQUIRE(cell != nullptr);
    return *cell;
}

/// @brief The cell at @p position, failing the running case first if it is outside @p buffer.
/// @param buffer The buffer to read.
/// @param position The cell's column (x) and row (y), 0-based.
/// @return The cell; the case has already ended when there is none.
[[nodiscard]] inline auto requireCell(Buffer const& buffer, Point position) -> Cell const&
{
    auto const* const cell = buffer.cellAt(position);
    REQUIRE(cell != nullptr);
    return *cell;
}

} // namespace core::tui::test
