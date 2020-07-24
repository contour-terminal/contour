#pragma once

#include <unordered_map>
#include <string>
#include <vector>

namespace crispy {

template <typename T>
constexpr bool ascending(T low, T val, T high) noexcept
{
    return low <= val && val <= high;
}

constexpr unsigned long strntoul(char const* _data, size_t _count, char const** _eptr, unsigned _base = 10)
{
    constexpr auto values = std::string_view{"0123456789ABCDEF"};
    constexpr auto lowerLetters = std::string_view{"abcdef"};

    unsigned long result = 0;
    while (_count != 0)
    {
        if (auto const i = values.find(*_data); i != values.npos && i < _base)
        {
            result *= _base;
            result += static_cast<unsigned long>(i);
            ++_data;
            --_count;
        }
        else if (auto const i = lowerLetters.find(*_data); i != lowerLetters.npos && _base == 16)
        {
            result *= _base;
            result += static_cast<unsigned long>(i);
            ++_data;
            --_count;
        }
        else
            return 0;
    }

    if (_eptr)
        *_eptr = _data;

    return result;
}

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

inline std::unordered_map<std::string_view, std::string_view> splitKeyValuePairs(std::string_view const&  _text, char _delimiter)
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
