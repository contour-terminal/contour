// SPDX-License-Identifier: Apache-2.0
#include <core/tui/HyperlinkEmitter.hpp>

#include <core/tui/TerminalOutput.hpp>

#include <algorithm>

namespace core::tui
{

auto HyperlinkIndex::at(int row, int col) const noexcept -> HyperlinkRegion const*
{
    auto const it = std::ranges::find_if(
        _regions, [row, col](HyperlinkRegion const& region) { return region.cellArea.contains(col, row); });
    return it != _regions.end() ? &*it : nullptr;
}

auto HyperlinkIndex::uriAt(int row, int col) const noexcept -> std::string_view
{
    auto const* region = at(row, col);
    return region != nullptr ? std::string_view { region->uri } : std::string_view {};
}

HyperlinkEmitter::HyperlinkEmitter(TerminalOutput& out, HyperlinkIndex const& index) noexcept:
    _out(&out), _index(&index)
{
}

HyperlinkEmitter::~HyperlinkEmitter()
{
    close();
}

void HyperlinkEmitter::syncTo(int row, int col)
{
    auto const* region = _index->at(row, col);
    if (region == _open)
        return;

    close();

    if (region != nullptr)
    {
        _out->beginHyperlink(region->uri, region->linkId);
        _open = region;
    }
}

void HyperlinkEmitter::close()
{
    if (_open == nullptr)
        return;

    _out->endHyperlink();
    _open = nullptr;
}

FlushLinks::FlushLinks(TerminalOutput& out, Buffer const& currentBuffer, Buffer const* previousBuffer):
    current(currentBuffer),
    previous(previousBuffer != nullptr ? HyperlinkIndex { *previousBuffer } : HyperlinkIndex {}),
    emitter(out, current),
    any(!current.empty() || !previous.empty())
{
}

} // namespace core::tui
