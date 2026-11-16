// SPDX-License-Identifier: Apache-2.0
#include <core/tui/Responsive.hpp>

#include <core/tui/layout/Style.hpp>

#include <ranges>
#include <utility>

namespace core::tui
{

Responsive::Responsive(std::vector<Breakpoint> breakpoints): _breakpoints(std::move(breakpoints))
{
}

Responsive::~Responsive()
{
    // Detach the layout while this object is still whole, rather than from Component's destructor.
    if (_current)
        removeChild(*_current);
}

void Responsive::render([[maybe_unused]] Canvas& canvas)
{
    // The chosen layout is a child; Screen::renderTree() renders it.
}

Size Responsive::preferredSize() const
{
    return _current ? _current->preferredSize() : Size {};
}

Size Responsive::minSize() const
{
    return _current ? _current->minSize() : Size {};
}

std::optional<std::size_t> Responsive::choose(std::vector<Breakpoint> const& breakpoints, Size size) noexcept
{
    auto chosen = std::optional<std::size_t> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, breakpoints.size()))
    {
        auto const& breakpoint = breakpoints[index];
        if (size.width >= breakpoint.minColumns && size.height >= breakpoint.minRows)
            chosen = index;
    }
    return chosen;
}

void Responsive::onResize(Size size)
{
    auto const next = choose(_breakpoints, size);
    if (next == _chosen)
        return;

    _chosen = next;
    // Retired rather than destroyed here: a focused component in it hears onBlur() once the change
    // closes, while it is still alive.
    if (_current)
        retireChild(std::move(_current));
    if (next && _breakpoints[*next].make)
        _current = _breakpoints[*next].make();
    // Basis 0 and grow 1: the layout fills the container whatever its own preferred size.
    if (_current)
        addChild(*_current, { .flex = layout::Style { .grow = 1, .basis = 0 } });

    // The layout's hints are this container's, so its parent sizes it afresh.
    hintsChanged();
}

} // namespace core::tui
