#pragma once

#include <crispy/defines.h>
#include <crispy/escape.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#if defined(CRISPY_CONCEPTS_SUPPORTED)
    #include <concepts>
#endif

namespace crispy
{

namespace views
{
    template <typename T>
    auto as()
    {
        return std::views::transform([](auto in) { return T(in); });
    }

    template <typename T>
    auto iota_as(int n)
    {
        return std::views::iota(0, n) | as<T>();
    }

    namespace detail
    {
        template <typename Range>
        std::string join_with_impl(Range&& range, std::string_view separator)
        {
            std::string result;
            auto it = std::begin(std::forward<Range>(range));
            auto const end = std::end(std::forward<Range>(range));

            if (it != end)
            {
                result += *it;
                ++it;
            }

            for (; it != end; ++it)
            {
                result += separator;
                result += *it;
            }
            return result;
        }

        struct join_with_fn
        {
            std::string_view separator;

            template <typename R>
            friend auto operator|(R&& r, join_with_fn const& self)
            {
                return join_with_impl(std::forward<R>(r), self.separator);
            }
        };
    } // namespace detail

    inline auto join_with(std::string_view sep)
    {
        return detail::join_with_fn { sep };
    }

    template <typename Range>
    auto join_with(Range&& range, std::string_view sep)
    {
        return detail::join_with_impl(std::forward<Range>(range), sep);
    }
    template <typename Range>
    struct enumerate_view_sentinel
    {
        using sentinel = std::ranges::sentinel_t<Range>;
        sentinel end;

        friend constexpr bool operator==(std::ranges::iterator_t<Range> const& it,
                                         enumerate_view_sentinel const& s)
        {
            return it == s.end;
        }
    };

    template <typename Range>
    struct enumerate_view
    {
        Range range;

        struct iterator
        {
            using range_iterator = std::ranges::iterator_t<Range>;
            using range_reference = std::ranges::range_reference_t<Range>;
            using difference_type = std::ptrdiff_t;
            using value_type = std::pair<size_t, std::ranges::range_value_t<Range>>;

            size_t index;
            range_iterator current;

            constexpr auto operator*() const
            {
                return std::pair<size_t, range_reference> { index, *current };
            }

            constexpr iterator& operator++()
            {
                ++index;
                ++current;
                return *this;
            }

            constexpr bool operator!=(iterator const& other) const { return current != other.current; }
            constexpr bool operator==(iterator const& other) const { return current == other.current; }
            constexpr bool operator!=(std::ranges::sentinel_t<Range> const& s) const { return current != s; }
            constexpr bool operator==(std::ranges::sentinel_t<Range> const& s) const { return current == s; }
        };

        constexpr auto begin() { return iterator { 0, std::begin(range) }; }
        constexpr auto end() { return std::end(range); }
    };

    struct enumerate_fn
    {
        template <typename Range>
        constexpr auto operator()(Range&& range) const
        {
            return enumerate_view<Range> { std::forward<Range>(range) };
        }
    };

    constexpr inline enumerate_fn enumerate; // NOLINT(readability-identifier-naming)
} // namespace views

using views::join_with;

constexpr std::string_view trimRight(std::string_view value) noexcept
{
    while (!value.empty())
    {
        if (std::string_view(" \t\r\n").find(value.back()) == std::string_view::npos)
            return value;
        value.remove_suffix(1);
    }
    return value;
}

template <typename T>
constexpr bool ascending(T low, T val, T high) noexcept
{
    return low <= val && val <= high;
}

constexpr unsigned long strntoul(char const* data, size_t count, char const** eptr, unsigned base = 10)
{
    constexpr auto Values = std::string_view { "0123456789ABCDEF" };
    constexpr auto LowerLetters = std::string_view { "abcdef" };

    unsigned long result = 0;
    while (count != 0)
    {
        if (auto const i = Values.find(*data); i != std::string_view::npos && i < base)
        {
            result *= base;
            result += static_cast<unsigned long>(i);
            ++data;
            --count;
        }
        else if (auto const i = LowerLetters.find(*data); i != std::string_view::npos && base == 16)
        {
            result *= base;
            result += static_cast<unsigned long>(i);
            ++data;
            --count;
        }
        else
            return 0;
    }

    if (eptr != nullptr)
        *eptr = data;

    return result;
}

template <typename T>
std::string joinHumanReadable(std::vector<T> const& list, std::string_view sep = ", ")
{
    std::stringstream result;
    for (size_t i = 0; i < list.size(); ++i)
    {
        if (i != 0)
            result << sep;
        result << std::format("{}", T(list[i]));
    }
    return result.str();
}

template <typename T, typename U>
std::string joinHumanReadableQuoted(std::vector<T> const& list, U sep = ", ")
{
    std::stringstream result;
    for (size_t i = 0; i < list.size(); ++i)
    {
        if (i != 0)
            result << sep;
        result << '"' << crispy::escape(std::format("{}", list[i])) << '"';
    }
    return result.str();
}

template <typename T, typename Callback>
constexpr inline bool split(std::basic_string_view<T> text, T delimiter, Callback const& callback)
{
    size_t a = 0;
    size_t b = 0;
    while ((b = text.find(delimiter, a)) != std::basic_string_view<T>::npos)
    {
        if (!(callback(text.substr(a, b - a))))
            return false;

        a = b + 1;
    }

    if (a < text.size())
        return callback(text.substr(a));

    return true;
}

template <typename T>
constexpr inline auto split(std::basic_string_view<T> text, T delimiter)
    -> std::vector<std::basic_string_view<T>>
{
    std::vector<std::basic_string_view<T>> output {};
    split(text, delimiter, [&](auto value) {
        output.emplace_back(value);
        return true;
    });
    return output;
}

template <typename T>
inline auto split(std::basic_string<T> const& text, T delimiter) -> std::vector<std::basic_string_view<T>>
{
    return split(std::basic_string_view<T>(text), delimiter);
}

inline std::unordered_map<std::string_view, std::string_view> splitKeyValuePairs(std::string_view const& text,
                                                                                 char delimiter)
{
    // params := pair (':' pair)*
    // pair := TEXT '=' TEXT

    // e.g.: foo=bar:foo2=bar2:....

    std::unordered_map<std::string_view, std::string_view> params;

    size_t iBeg = 0;
    size_t i = text.find(delimiter);

    // e.g.: foo=bar::foo2=bar2:....
    while (i != std::string_view::npos)
    {
        auto const param = std::string_view(text.data() + iBeg, i - iBeg);
        if (auto const k = param.find('='); k != std::string_view::npos)
        {
            auto const key = param.substr(0, k);
            auto const val = param.substr(k + 1);
            if (!key.empty())
                params[key] = val;
        }
        iBeg = i + 1;
        i = text.find(delimiter, iBeg);
    }

    auto const param = std::string_view(text.data() + iBeg);
    if (auto const k = param.find('='); k != std::string_view::npos)
    {
        auto const key = param.substr(0, k);
        auto const val = param.substr(k + 1);
        if (!key.empty())
            params[key] = val;
    }

    return params;
}

template <typename Ch>
bool startsWith(std::basic_string_view<Ch> text, std::basic_string_view<Ch> prefix)
{
    if (text.size() < prefix.size())
        return false;

    for (size_t i = 0; i < prefix.size(); ++i)
        if (text[i] != prefix[i])
            return false;

    return true;
}

template <typename Ch>
bool endsWith(std::basic_string_view<Ch> text, std::basic_string_view<Ch> prefix)
{
    if (text.size() < prefix.size())
        return false;

    for (size_t i = 0; i < prefix.size(); ++i)
        if (text[text.size() - prefix.size() + i] != prefix[i])
            return false;

    return true;
}

template <std::size_t Base = 10, typename T = unsigned, typename C>
constexpr std::optional<T> to_integer(std::basic_string_view<C> text) noexcept
{
    static_assert(Base == 2 || Base == 8 || Base == 10 || Base == 16, "Only base-2/8/10/16 supported.");
    static_assert(std::is_integral_v<T>, "T must be an integral type.");
    static_assert(std::is_integral_v<C>, "C must be an integral type.");

    if (text.empty())
        return std::nullopt;

    auto value = T { 0 };

    for (auto const ch: text)
    {
        value = static_cast<T>(value * static_cast<T>(Base));
        switch (Base)
        {
            case 2:
                if ('0' <= ch && ch <= '1')
                    value = T(value + T(ch - '0'));
                else
                    return std::nullopt;
                break;
            case 8:
                if ('0' <= ch && ch <= '7')
                    value = T(value + T(ch - '0'));
                else
                    return std::nullopt;
                break;
            case 10:
                if ('0' <= ch && ch <= '9')
                    value = T(value + T(ch - '0'));
                else
                    return std::nullopt;
                break;
            case 16:
                if ('0' <= ch && ch <= '9')
                    value = T(value + T(ch - '0'));
                else if ('a' <= ch && ch <= 'f')
                    value = T(value + T(10 + ch - 'a'));
                else if (ch >= 'A' && ch <= 'F')
                    value = T(value + T(10 + ch - 'A'));
                else
                    return std::nullopt;
                break;
        }
    }

    return value;
}

template <std::size_t Base = 10, typename T = unsigned, typename C>
constexpr std::optional<T> to_integer(std::basic_string<C> text) noexcept
{
    return to_integer<Base, T, C>(std::basic_string_view<C>(text));
}

class finally // NOLINT(readability-identifier-naming)
{
  public:
    explicit finally(std::function<void()> hook): _hook(std::move(hook)) {}

    finally(finally const&) = delete;
    finally& operator=(finally const&) = delete;
    finally(finally&&) = delete;
    finally& operator=(finally&&) = delete;

    void run()
    {
        if (_hook)
        {
            auto hooked = std::move(_hook);
            _hook = {};
            hooked();
        }
    }

    ~finally() { run(); }

  private:
    std::function<void()> _hook {};
};

#if defined(CRISPY_CONCEPTS_SUPPORTED)

// clang-format off
template <typename T>
concept LockableConcept = requires(T t)
{
    { t.lock() } -> std::same_as<void>;
    { t.unlock() } -> std::same_as<void>;
};
// clang-format on

#endif

template <typename L, typename F>
CRISPY_REQUIRES(LockableConcept<L>)
auto locked(L& lockable, F const& f)
{
    auto const _ = std::scoped_lock { lockable };
    return f();
}

inline std::optional<unsigned> fromHexDigit(char value)
{
    if ('0' <= value && value <= '9')
        return value - '0';
    if ('a' <= value && value <= 'f')
        return 10 + value - 'a';
    if ('A' <= value && value <= 'F')
        return 10 + value - 'A';
    return std::nullopt;
}

template <typename T>
std::optional<std::basic_string<T>> fromHexString(std::basic_string_view<T> hexString)
{
    if (hexString.size() % 2)
        return std::nullopt;

    std::basic_string<T> output;
    output.resize(hexString.size() / 2);

    auto i = hexString.rbegin();
    auto e = hexString.rend();
    size_t k = output.size();
    while (i != e)
    {
        auto const c1 = fromHexDigit(*i++);
        auto const c2 = fromHexDigit(*i++);
        if (!c1 || !c2)
            return std::nullopt;
        auto const value = (T) (c2.value() << 4 | c1.value());
        output[--k] = value;
    }

    return output;
}

template <typename T>
std::basic_string<T> toHexString(std::basic_string_view<T> input)
{
    std::basic_string<T> output;

    for (T const ch: input)
        output += std::format("{:02X}", static_cast<unsigned>(ch));

    return output;
}

template <typename T>
inline std::basic_string<T> toLower(std::basic_string_view<T> value)
{
    std::basic_string<T> result;
    result.reserve(value.size());
    transform(begin(value), end(value), back_inserter(result), [](auto ch) { return tolower(ch); });
    return result;
}

template <typename T>
inline std::basic_string<T> toLower(std::basic_string<T> const& value)
{
    return toLower<T>(std::basic_string_view<T>(value));
}

template <typename T>
inline std::basic_string<T> toUpper(std::basic_string_view<T> value)
{
    std::basic_string<T> result;
    result.reserve(value.size());
    std::transform(begin(value), end(value), back_inserter(result), [](auto ch) { return std::toupper(ch); });
    return result;
}

template <typename T>
inline std::basic_string<T> toUpper(std::basic_string<T> const& value)
{
    return toUpper<T>(std::basic_string_view<T>(value));
}

inline std::string readFileAsString(std::filesystem::path const& path)
{
    auto const fileSize = std::filesystem::file_size(path);
    auto text = std::string();
    text.resize(fileSize);
    std::ifstream in(path.string());
    in.read(text.data(), static_cast<std::streamsize>(fileSize));
    return text;
}

/// Constructs a container to conveniently iterate over all elements
/// of the template type.
///
/// Any type is supported that can be iterated,
/// and has a specialization for std::numeric_limits<T>.
template <typename T>
constexpr auto each_element() noexcept
{
    struct container
    {
        struct iterator // NOLINT(readability-identifier-naming)
        {
            T value;
            constexpr T& operator*() noexcept { return value; }
            constexpr T const& operator*() const noexcept { return value; }
            constexpr iterator& operator++() noexcept
            {
                value = static_cast<T>(static_cast<int>(value) + 1);
                return *this;
            }
            constexpr bool operator==(iterator other) noexcept { return value == other.value; }
            constexpr bool operator!=(iterator other) noexcept { return value != other.value; }
        };
        constexpr iterator begin() noexcept { return iterator { std::numeric_limits<T>::min() }; }
        constexpr iterator end() noexcept
        {
            return iterator { static_cast<T>(static_cast<int>(std::numeric_limits<T>::max()) + 1) };
        }
    };
    return container {};
}

template <typename T>
inline std::string replace(std::string_view text, std::string_view pattern, T&& value)
{
    auto i = text.find(pattern);
    if (i == std::string_view::npos)
        return std::string(text);

    std::ostringstream os;
    os << text.substr(0, i);
    os << std::forward<T>(value);
    os << text.substr(i + pattern.size());
    return os.str();
}

inline std::filesystem::path homeResolvedPath(std::string input, const std::filesystem::path& homeDirectory)
{
    if (!input.empty() && input[0] == '~')
    {
        bool const pathSepFound = input.size() >= 2 && (input[1] == '/' || input[1] == '\\');
        auto subPath = input.substr(pathSepFound ? 2 : 1);
        return homeDirectory / std::filesystem::path(subPath);
    }

    return std::filesystem::path(input);
}

template <typename VariableReplacer>
inline std::string replaceVariables(std::string_view text, VariableReplacer replace)
{
    using namespace std::string_view_literals;

    auto output = std::string {};
    auto constexpr Npos = std::string_view::npos;
    auto i = std::string_view::size_type { 0 };

    auto constexpr MarkerStart = "${"sv;
    auto constexpr MarkerEnd = "}"sv;

    while (i != Npos)
    {
        auto const markerStartOffset = text.find(MarkerStart, i);
        if (markerStartOffset == Npos)
            break;

        auto const gapText = text.substr(i, markerStartOffset - i);
        output += gapText;

        auto const markerEndOffset = text.find(MarkerEnd, markerStartOffset + MarkerStart.size());
        if (markerEndOffset == Npos)
            break; // Invalid variable format. Closing variable marker not found.

        auto const nameLength = markerEndOffset - (markerStartOffset + MarkerStart.size());
        auto const name = text.substr(markerStartOffset + MarkerStart.size(), nameLength);
        output += replace(name);

        i = markerEndOffset + MarkerEnd.size();
    }
    output += text.substr(i);

    return output;
}

// https://graphics.stanford.edu/~seander/bithacks.html#RoundUpPowerOf2
// 1U << (lg(v - 1) + 1)
template <typename T>
constexpr T nextPowerOfTwo(T v) noexcept
{
    static_assert(std::is_integral_v<T>);
    static_assert(std::is_unsigned_v<T>);

    // return 1U << (std::log(v - 1) + 1);
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    if constexpr (sizeof(T) >= 16)
        v |= v >> 8;
    if constexpr (sizeof(T) >= 32)
        v |= v >> 16;
    if constexpr (sizeof(T) >= 64)
        v |= v >> 32;
    v++;
    return v;
}

inline std::string humanReadableBytes(uint64_t bytes)
{
    if (bytes <= 1024)
        return std::format("{} bytes", unsigned(bytes));

    auto const kb = static_cast<long double>(bytes) / 1024.0;
    if (kb <= 1024.0)
        return std::format("{:.03} KB", kb);

    auto const mb = kb / 1024.0;
    if (mb <= 1024.0)
        return std::format("{:.03} MB", mb);

    auto const gb = mb / 1024.0;
    return std::format("{:.03} GB", gb);
}

template <typename... Ts>
constexpr void ignore_unused(Ts... /*values*/) noexcept
{
}

std::string threadName();

template <class... Ts>
struct overloaded: Ts...
{
    using Ts::operator()...;
};

template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

template <typename T, typename... Ts>
concept one_of = (std::same_as<T, Ts> || ...);

} // namespace crispy
