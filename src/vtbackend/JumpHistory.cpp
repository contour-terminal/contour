// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/JumpHistory.h>

namespace vtbackend
{

CellLocation JumpHistory::jumpToLast(CellLocation current)
{
    applyOffset();
    CellLocation last = _history.back();
    if (last == current)
    {
        _history.pop_back();
        if (_history.empty())
        {
            return current;
        }
        last = _history.back();
    }
    _history.pop_back();
    _history.push_back(current);
    _current = _history.size();
    return last;
}

CellLocation JumpHistory::jumpToMarkBackward([[maybe_unused]] CellLocation current)
{
    applyOffset();
    if (_current == 0)
    {
        // loop
        _current = _history.size() - 1;
    }
    else
    {
        _current--;
    }
    return _history[_current];
}

CellLocation JumpHistory::jumpToMarkForward([[maybe_unused]] CellLocation current)
{
    applyOffset();
    if (_current == _history.size())
    {
        // loop
        _current = 0;
    }
    else
    {
        _current++;
    }
    return _history[_current];
}

} // namespace vtbackend
