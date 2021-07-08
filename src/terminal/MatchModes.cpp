#include <terminal/MatchModes.h>
#include <terminal/Terminal.h>

namespace terminal {

MatchModes constructMatchModes(Terminal const& _terminal)
{
    auto mm = MatchModes{};
    if (_terminal.screen().isAlternateScreen())
        mm.enable(MatchModes::AlternateScreen);
    if (_terminal.applicationCursorKeys())
        mm.enable(MatchModes::AppCursor);
    if (_terminal.applicationKeypad())
        mm.enable(MatchModes::AppKeypad);
    if (_terminal.selectionAvailable())
        mm.enable(MatchModes::Select);
    return mm;
}

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
        case MatchModes::Status::Any:
            break;
    }

    //TODO: _mode.status(MatchModes::AppCursor);
    //TODO: _mode.status(MatchModes::AppKeypad);

    return false;
}

}
