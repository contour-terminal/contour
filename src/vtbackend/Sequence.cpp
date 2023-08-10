#include <vtbackend/ControlCode.h>
#include <vtbackend/Sequence.h>

#include <crispy/escape.h>

#include <numeric>
#include <sstream>
#include <string>

using std::accumulate;
using std::string;
using std::stringstream;

namespace terminal
{

std::string sequence::raw() const
{
    stringstream sstr;

    switch (_category)
    {
        case function_category::C0: break;
        case function_category::ESC: sstr << "\033"; break;
        case function_category::CSI: sstr << "\033["; break;
        case function_category::DCS: sstr << "\033P"; break;
        case function_category::OSC: sstr << "\033]"; break;
    }

    // if (parameterCount() > 1 || (parameterCount() == 1 && _parameters.at(0) != 0))
    {
        for (size_t i = 0; i < parameterCount(); ++i)
        {
            if (i)
                sstr << ';';

            sstr << param(i);
            for (size_t k = 1; k < subParameterCount(i); ++k)
                sstr << ':' << subparam(i, k);
        }
    }

    sstr << intermediateCharacters();

    if (_finalChar)
        sstr << _finalChar;

    if (!_dataString.empty())
        sstr << _dataString << "\033\\";

    return sstr.str();
}

string sequence::text() const
{
    stringstream sstr;

    if (_category == function_category::C0)
    {
        sstr << to_short_string(ControlCode::C0(_finalChar));
        return sstr.str();
    }

    sstr << fmt::format("{}", _category);

    if (_leaderSymbol)
        sstr << ' ' << _leaderSymbol;

    if (parameterCount() > 1 || (parameterCount() == 1 && _parameters.at(0) != 0))
        sstr << ' ' << _parameters.str();

    if (!intermediateCharacters().empty())
        sstr << ' ' << intermediateCharacters();

    if (_finalChar)
        sstr << ' ' << _finalChar;

    if (!_dataString.empty())
        sstr << " \"" << crispy::escape(_dataString) << "\" ST";

    return sstr.str();
}

} // namespace terminal
