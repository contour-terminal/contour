// SPDX-License-Identifier: Apache-2.0
#include <vtpty/StreamKeeper.hpp>

#include <algorithm>
#include <utility>

namespace vtpty
{

StreamKeeper::StreamKeeper(std::size_t cap, std::vector<std::string> droppedPrefixes):
    _cap { cap },
    _droppedPrefixes { std::move(droppedPrefixes) },
    _state { _droppedPrefixes.empty() ? LineState::Keeping : LineState::Deciding },
    _lineStart {},
    _captured {}
{
}

void StreamKeeper::keep(std::string_view bytes)
{
    auto const kept = std::min(bytes.size(), _cap - std::min(_cap, _captured.bytes.size()));
    _captured.bytes.append(bytes.substr(0, kept));
    if (kept < bytes.size())
        _captured.extent = OutputExtent::Truncated;
}

void StreamKeeper::decide()
{
    if (std::ranges::contains(_droppedPrefixes, _lineStart))
    {
        _state = LineState::Dropping;
        _lineStart.clear();
        return;
    }
    auto const couldBecome = [this](std::string const& prefix) {
        return prefix.starts_with(_lineStart);
    };
    if (std::ranges::none_of(_droppedPrefixes, couldBecome))
    {
        _state = LineState::Keeping;
        keep(_lineStart);
        _lineStart.clear();
    }
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
            _lineStart.push_back(byte);
            if (byte != '\n')
            {
                decide();
                continue;
            }
            // The line ended shorter than every prefix it could have become.
            keep(_lineStart);
            _lineStart.clear();
            continue;
        }
        auto const end = bytes.find('\n');
        auto const length = end == std::string_view::npos ? bytes.size() : end + 1;
        if (_state == LineState::Keeping)
            keep(bytes.substr(0, length));
        bytes.remove_prefix(length);
        if (end != std::string_view::npos)
            _state = LineState::Deciding;
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
