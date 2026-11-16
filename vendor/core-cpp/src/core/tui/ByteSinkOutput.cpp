// SPDX-License-Identifier: Apache-2.0
#include <core/tui/ByteSinkOutput.hpp>

#include <utility>

namespace core::tui
{

ByteSinkOutput::ByteSinkOutput(ByteSink sink, Size size, SinkDestination destination) noexcept:
    _sink(std::move(sink)), _size(size), _destination(destination)
{
}

auto ByteSinkOutput::initialize() -> VoidResult
{
    return {};
}

void ByteSinkOutput::detectCapabilities()
{
}

bool ByteSinkOutput::isTerminal() const noexcept
{
    return _destination == SinkDestination::Terminal;
}

auto ByteSinkOutput::columns() const noexcept -> int
{
    return _size.width;
}

auto ByteSinkOutput::rows() const noexcept -> int
{
    return _size.height;
}

void ByteSinkOutput::updateDimensions()
{
}

void ByteSinkOutput::resize(Size size) noexcept
{
    _size = size;
}

void ByteSinkOutput::writeToDestination(std::string_view bytes)
{
    _sink(bytes);
}

} // namespace core::tui
