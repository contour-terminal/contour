#pragma once

#include <unordered_map>
#include <string>
#include <vector>

namespace crispy {

inline std::vector<std::string_view> split(std::string_view const&  _text, char _delimiter)
{
    auto res = std::vector<std::string_view>{};

    size_t i_beg = 0;
    size_t i = _text.find(_delimiter);

    // e.g.: foobar::foo2:foo3:....
    while (i != _text.npos)
    {
        res.push_back(std::string_view(_text.data() + i_beg, i - i_beg));

        i_beg = i + 1;
        i = _text.find(_delimiter, i_beg);
    }
    res.push_back(std::string_view{_text.data() + i_beg, _text.size() - i_beg});
    return res;
}

std::unordered_map<std::string_view, std::string_view> splitKeyValuePairs(std::string_view const&  _text, char _delimiter)
{
    // params := pair (':' pair)*
    // pair := TEXT '=' TEXT

    // e.g.: foo=bar:foo2=bar2:....

    std::unordered_map<std::string_view, std::string_view> params;

    size_t i_beg = 0;
    size_t i = _text.find(_delimiter);

    // e.g.: foo=bar::foo2=bar2:....
    while (i != _text.npos)
    {
        std::string_view param(_text.data() + i_beg, i - i_beg);
        if (auto const k = param.find('='); k != param.npos)
        {
            auto const key = param.substr(0, k);
            auto const val = param.substr(k + 1);
            if (!key.empty())
                params[key] = val;
        }
        i_beg = i + 1;
        i = _text.find(_delimiter, i_beg);
    }

    std::string_view param(_text.data() + i_beg);
    if (auto const k = param.find('='); k != param.npos)
    {
        auto const key = param.substr(0, k);
        auto const val = param.substr(k + 1);
        if (!key.empty())
            params[key] = val;
    }

    return params;
}

} // end namespace
