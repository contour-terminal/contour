#include <terminal/MatchModes.h>
#include <terminal/Terminal.h>

namespace terminal
{

bool testMatch(Terminal const& _terminal, MatchModes _mode)
{
    switch (_mode.status(MatchModes::AlternateScreen))
    {
    case MatchModes::Status::Enabled:
        if (!_terminal.screen().isAlternateScreen())
            return false;
        break;
    case MatchModes::Status::Disabled:
        if (_terminal.screen().isAlternateScreen())
            return false;
        break;
    case MatchModes::Status::Any: break;
    }

    // TODO: _mode.status(MatchModes::AppCursor);
    // TODO: _mode.status(MatchModes::AppKeypad);

    return false;
}

} // namespace terminal
