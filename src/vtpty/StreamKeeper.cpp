// SPDX-License-Identifier: Apache-2.0
#include <vtpty/StreamKeeper.hpp>

#include <core/Assert.hpp>

#include <algorithm>
#include <ranges>
#include <utility>

namespace vtpty
{

StreamKeeper::StreamKeeper(std::size_t cap, std::vector<std::string> droppedPrefixes):
    _cap { cap },
    _droppedPrefixes { std::move(droppedPrefixes) },
    _state { _droppedPrefixes.empty() ? LineState::Keeping : LineState::Deciding },
    _live {},
    _lineStart {},
    _captured {}
{
    Require(std::ranges::none_of(_droppedPrefixes, [](std::string const& prefix) { return prefix.empty(); }));
    if (!_droppedPrefixes.empty())
        startLine();
}

void StreamKeeper::startLine()
{
    _state = LineState::Deciding;
    _live.assign(_droppedPrefixes.size(), Live::Yes);
    _lineStart.clear();
}

void StreamKeeper::keep(std::string_view bytes)
{
    auto const kept = std::min(bytes.size(), _cap - std::min(_cap, _captured.bytes.size()));
    _captured.bytes.append(bytes.substr(0, kept));
    if (kept < bytes.size())
        _captured.extent = OutputExtent::Truncated;
}

void StreamKeeper::decide(char byte)
{
    // Only the new byte is compared, and only with the prefixes the line still matches: a line costs at most
    // the prefixes times the longest of them, however long it is.
    auto const at = _lineStart.size();
    _lineStart.push_back(byte);
    auto anyLive = false;
    for (auto const index: std::views::iota(std::size_t { 0 }, _droppedPrefixes.size()))
    {
        if (_live[index] == Live::No)
            continue;
        auto const& prefix = _droppedPrefixes[index];
        ++_comparisons;
        if (prefix[at] != byte)
        {
            _live[index] = Live::No;
            continue;
        }
        if (prefix.size() == at + 1)
        {
            _state = LineState::Dropping;
            _lineStart.clear();
            return;
        }
        anyLive = true;
    }
    if (anyLive)
        return;
    _state = LineState::Keeping;
    keep(_lineStart);
    _lineStart.clear();
}

void StreamKeeper::take(std::string_view bytes)
{
    if (_droppedPrefixes.empty())
    {
        keep(bytes);
        return;
    }
    while (!bytes.empty())
    {
        if (_state == LineState::Deciding)
        {
            auto const byte = bytes.front();
            bytes.remove_prefix(1);
            if (byte != '\n')
            {
                decide(byte);
                continue;
            }
            // The line ended shorter than every prefix it could have become.
            _lineStart.push_back(byte);
            keep(_lineStart);
            startLine();
            continue;
        }
        auto const end = bytes.find('\n');
        auto const length = end == std::string_view::npos ? bytes.size() : end + 1;
        if (_state == LineState::Keeping)
            keep(bytes.substr(0, length));
        bytes.remove_prefix(length);
        if (end != std::string_view::npos)
            startLine();
    }
}

CapturedStream StreamKeeper::finish() &&
{
    // A last line still Deciding is shorter than the prefixes it could have become, and none of them.
    if (_state == LineState::Deciding && !_lineStart.empty())
        keep(_lineStart);
    _lineStart.clear();
    return std::move(_captured);
}

} // namespace vtpty
