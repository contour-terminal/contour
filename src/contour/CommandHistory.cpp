// SPDX-License-Identifier: Apache-2.0
#include <contour/CommandHistory.h>

#include <algorithm>
#include <string>

namespace contour
{

void CommandHistory::record(std::string_view id)
{
    if (_capacity == 0 || id.empty())
        return;

    // Move-to-front: an id already remembered is re-ordered, never duplicated. Erasing first (rather
    // than rotating in place) keeps this correct whether or not the id was present.
    auto const existing = std::ranges::find(_recent, id);
    if (existing != _recent.end())
        _recent.erase(existing);

    _recent.insert(_recent.begin(), std::string(id));
    trim();
}

void CommandHistory::reset(std::span<std::string const> ids)
{
    _recent.assign(ids.begin(), ids.end());
    trim();
}

void CommandHistory::setCapacity(std::size_t capacity)
{
    _capacity = capacity;
    trim();
}

void CommandHistory::trim()
{
    if (_recent.size() > _capacity)
        _recent.resize(_capacity);
}

} // namespace contour
