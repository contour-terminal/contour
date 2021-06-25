#pragma once

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

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

template <typename T, typename Callback>
constexpr inline bool split(std::basic_string_view<T> _text,
                            T _delimiter,
                            Callback&& _callback)
{
    size_t a = 0;
    size_t b = 0;
    while ((b = _text.find(_delimiter, a)) != std::basic_string_view<T>::npos)
    {
        if (!(_callback(_text.substr(a, b - a))))
            return false;

        a = b + 1;
    }

    if (a < _text.size())
        return _callback(_text.substr(a));

    return true;
}

template <typename T>
constexpr inline auto split(std::basic_string_view<T> _text, T _delimiter) -> std::vector<std::basic_string_view<T>>
{
    std::vector<std::basic_string_view<T>> output{};
    split(_text, _delimiter, [&](auto value) { output.emplace_back(value); return true; });
    return output;
}

template <typename T>
inline auto split(std::basic_string<T> const& _text, T _delimiter) -> std::vector<std::basic_string_view<T>>
{
    return split(std::basic_string_view<T>(_text), _delimiter);
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

template <typename Ch>
bool startsWith(std::basic_string_view<Ch> _text, std::basic_string_view<Ch> _prefix)
{
    if (_text.size() < _prefix.size())
        return false;

    for (size_t i = 0; i < _prefix.size(); ++i)
        if (_text[i] != _prefix[i])
            return false;

    return true;
}

template <typename Ch>
bool endsWith(std::basic_string_view<Ch> _text, std::basic_string_view<Ch> _prefix)
{
    if (_text.size() < _prefix.size())
        return false;

    for (size_t i = 0; i < _prefix.size(); ++i)
        if (_text[_text.size() - _prefix.size() + i] != _prefix[i])
            return false;

    return true;
}

template <std::size_t Base = 10, typename T = unsigned, typename C>
constexpr std::optional<T> to_integer(std::basic_string_view<C> _text) noexcept
{
    static_assert(Base == 2 || Base == 8 || Base == 10 || Base == 16, "Only base-2/8/10/16 supported.");
    static_assert(std::is_integral_v<T>, "T must be an integral type.");
    static_assert(std::is_integral_v<C>, "C must be an integral type.");

    if (_text.empty())
        return std::nullopt;

    auto value = T{0};

    for (auto const ch: _text)
    {
        value *= Base;
        switch (Base)
        {
            case 2:
                if ('0' <= ch && ch <= '1')
                    value += static_cast<T>(ch - '0');
                else
                    return std::nullopt;
                break;
            case 8:
                if ('0' <= ch && ch <= '7')
                    value += static_cast<T>(ch - '0');
                else
                    return std::nullopt;
                break;
            case 10:
                if ('0' <= ch && ch <= '9')
                    value += static_cast<T>(ch - '0');
                else
                    return std::nullopt;
                break;
            case 16:
                if ('0' <= ch && ch <= '9')
                    value += static_cast<T>(ch - '0');
                else if ('a' <= ch && ch <= 'f')
                    value += static_cast<T>(10 + ch - 'a');
                else if (ch >= 'A' && ch <= 'F')
                    value += static_cast<T>(10 + ch - 'A');
                else
                    return std::nullopt;
                break;
        }
    }

    return value;
}

template <std::size_t Base = 10, typename T = unsigned, typename C>
constexpr std::optional<T> to_integer(std::basic_string<C> _text) noexcept
{
    return to_integer<Base, T, C>(std::basic_string_view<C>(_text));
}

struct finally {
    std::function<void()> hook{};
    ~finally() { hook(); }
};

inline std::optional<unsigned> fromHexDigit(char _value)
{
    if ('0' <= _value && _value <= '9')
        return _value - '0';
    if ('a' <= _value && _value <= 'f')
        return 10 + _value - 'a';
    if ('A' <= _value && _value <= 'F')
        return 10 + _value - 'A';
    return std::nullopt;
}

template <typename T>
std::optional<std::basic_string<T>> fromHexString(std::basic_string_view<T> _hexString)
{
    if (_hexString.size() % 2)
        return std::nullopt;

    std::basic_string<T> output;
    output.resize(_hexString.size() / 2);

    auto i = _hexString.rbegin();
    auto e = _hexString.rend();
    size_t k = output.size();
    while (i != e)
    {
        auto const c1 = fromHexDigit(*i++);
        auto const c2 = fromHexDigit(*i++);
        if (!c1 || !c2)
            return std::nullopt;
        auto const value = static_cast<char>((c2.value() << 4 | c1.value()));
        output[--k] = value;
    }

    return output;
}

template <typename T>
std::basic_string<T> toHexString(std::basic_string_view<T> _input)
{
    std::basic_string<T> output;

    for (T const ch: _input)
        output += fmt::format("{:02X}", static_cast<unsigned>(ch));

    return output;
}

} // end namespace
