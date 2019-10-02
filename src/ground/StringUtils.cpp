#include <ground/StringUtils.h>
#include <cstdlib>

using namespace std;

namespace ground {

string parseEscaped(string const& _value) {
    string out;
    out.reserve(_value.size());

    enum class State { Text, Escape, Octal1, Octal2, Hex1, Hex2 };
    State state = State::Text;
    char buf[3] = {};

    for (size_t i = 0; i < _value.size(); ++i)
    {
        switch (state)
        {
            case State::Text:
                if (_value[i] == '\\')
                    state = State::Escape;
                else
                    out.push_back(_value[i]);
                break;
            case State::Escape:
                if (_value[i] == '0')
                    state = State::Octal1;
                else if (_value[i] == 'x')
                    state = State::Hex1;
                else
                {
                    // Unknown escape sequence, so just continue as text.
                    out.push_back('\\');
                    out.push_back(_value[i]);
                    state = State::Text;
                }
                break;
            case State::Octal1:
                buf[0] = _value[i];
                state = State::Octal2;
                break;
            case State::Octal2:
                buf[1] = _value[i];
				out.push_back(static_cast<char>(strtoul(buf, nullptr, 8)));
                state = State::Text;
                break;
            case State::Hex1:
                buf[0] = _value[i];
                state = State::Hex2;
                break;
            case State::Hex2:
                buf[1] = _value[i];
				out.push_back(static_cast<char>(strtoul(buf, nullptr, 16)));
                state = State::Text;
                break;
        }
    }

    return out;

}

} // namespace ground
