#include <terminal/Sequence.h>

#include <crispy/escape.h>

#include <numeric>
#include <sstream>
#include <string>

using std::accumulate;
using std::string;
using std::stringstream;

namespace terminal
{

std::string Sequence::raw() const
{
    stringstream sstr;

    switch (category_)
    {
        case FunctionCategory::C0: break;
        case FunctionCategory::ESC: sstr << "\033"; break;
        case FunctionCategory::CSI: sstr << "\033["; break;
        case FunctionCategory::DCS: sstr << "\033P"; break;
        case FunctionCategory::OSC: sstr << "\033]"; break;
    }

    // if (parameterCount() > 1 || (parameterCount() == 1 && parameters_.at(0) != 0))
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

    if (finalChar_)
        sstr << finalChar_;

    if (!dataString_.empty())
        sstr << dataString_ << "\033\\";

    return sstr.str();
}

string Sequence::text() const
{
    stringstream sstr;

    sstr << fmt::format("{}", category_);

    if (leaderSymbol_)
        sstr << ' ' << leaderSymbol_;

    if (parameterCount() > 1 || (parameterCount() == 1 && parameters_.at(0) != 0))
        sstr << ' ' << parameters_.str();

    if (!intermediateCharacters().empty())
        sstr << ' ' << intermediateCharacters();

    if (finalChar_)
        sstr << ' ' << finalChar_;

    if (!dataString_.empty())
        sstr << " \"" << crispy::escape(dataString_) << "\" ST";

    return sstr.str();
}

} // namespace terminal
