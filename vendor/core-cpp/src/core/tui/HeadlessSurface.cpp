// SPDX-License-Identifier: Apache-2.0
#include <core/tui/HeadlessSurface.hpp>

#include <utility>

namespace core::tui
{

HeadlessSurface::HeadlessSurface(Size size, ByteSink sink, SinkDestination destination) noexcept:
    _output(std::move(sink), size, destination)
{
}

auto HeadlessSurface::columns() const noexcept -> int
{
    return _output.columns();
}

auto HeadlessSurface::rows() const noexcept -> int
{
    return _output.rows();
}

auto HeadlessSurface::output() noexcept -> TerminalOutput&
{
    return _output;
}

auto HeadlessSurface::queryCursorPosition() -> std::expected<std::pair<int, int>, QueryUnanswered>
{
    return std::unexpected { QueryUnanswered::NotAsked };
}

void HeadlessSurface::resize(Size size) noexcept
{
    _output.resize(size);
}

} // namespace core::tui
