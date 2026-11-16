// SPDX-License-Identifier: Apache-2.0
#include <core/tui/MockScreenSurface.hpp>

namespace core::tui
{

MockScreenSurface::MockScreenSurface(): _output(std::make_unique<MockTerminalOutput>())
{
}

MockScreenSurface::MockScreenSurface(std::unique_ptr<MockTerminalOutput> output): _output(std::move(output))
{
}

auto MockScreenSurface::columns() const noexcept -> int
{
    return _output->columns();
}

auto MockScreenSurface::rows() const noexcept -> int
{
    return _output->rows();
}

auto MockScreenSurface::output() noexcept -> TerminalOutput&
{
    return *_output;
}

auto MockScreenSurface::queryCursorPosition() -> std::expected<std::pair<int, int>, QueryUnanswered>
{
    // MockTerminalOutput answers its cursor, but a subclass of it may override the answer away;
    // that is a surface with no cursor to report, which the inline viewport already handles.
    if (auto const position = _output->simulatedCursorPosition())
        return *position;
    return std::unexpected(QueryUnanswered::NotAsked);
}

} // namespace core::tui
