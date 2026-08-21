#pragma once

#include <crispy/Defines.hpp>
#include <crispy/Escape.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef CRISPY_CONCEPTS_SUPPORTED
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
    auto iotaAs(int n)
    {
        return std::views::iota(0, n) | as<T>();
    }

    namespace detail
    {
        template <typename Range>
        std::string joinWithImpl(Range const& range, std::string_view separator)
        {
            std::string result;
            auto it = std::begin(range);
            auto const end = std::end(range);

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

        struct JoinWithFn
        {
            std::string_view separator;

            template <typename R>
            friend auto operator|(R&& r, JoinWithFn const& self)
            {
                return joinWithImpl(std::forward<R>(r), self.separator);
            }
        };
    } // namespace detail

    inline auto joinWith(std::string_view sep)
    {
        return detail::JoinWithFn { sep };
    }

    template <typename Range>
    auto joinWith(Range&& range, std::string_view sep)
    {
        return detail::joinWithImpl(std::forward<Range>(range), sep);
    }
    template <typename Range>
    struct EnumerateViewSentinel
    {
        // NOLINTNEXTLINE(readability-identifier-naming): standard iterator/container trait spelling.
        using sentinel = std::ranges::sentinel_t<Range>;
        sentinel end;

        friend constexpr bool operator==(std::ranges::iterator_t<Range> const& it,
                                         EnumerateViewSentinel const& s)
        {
            return it == s.end;
        }
    };

    template <typename Range>
    struct EnumerateView
    {
        Range range;

        // NOLINTNEXTLINE(readability-identifier-naming): standard iterator/container trait spelling.
        struct iterator
        {
            using RangeIterator = std::ranges::iterator_t<Range>;
            using RangeReference = std::ranges::range_reference_t<Range>;
            // NOLINTBEGIN(readability-identifier-naming): standard iterator/container trait spelling.
            using difference_type = std::ptrdiff_t;
            using value_type = std::pair<size_t, std::ranges::range_value_t<Range>>;
            // NOLINTEND(readability-identifier-naming)

            size_t index;
            RangeIterator current;

            constexpr auto operator*() const { return std::pair<size_t, RangeReference> { index, *current }; }

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

    struct EnumerateFn
    {
        template <typename Range>
        constexpr auto operator()(Range&& range) const
        {
            return EnumerateView<Range> { std::forward<Range>(range) };
        }
    };

    constexpr inline EnumerateFn enumerate; // NOLINT(readability-identifier-naming)
} // namespace views

using views::joinWith;

/// The ASCII whitespace trimRight(), trimLeft() and trim() strip.
constexpr auto Whitespace = std::string_view { " \t\r\n" };

/// @return @p value without its trailing whitespace.
constexpr std::string_view trimRight(std::string_view value) noexcept
{
    while (!value.empty())
    {
        if (!Whitespace.contains(value.back()))
            return value;
        value.remove_suffix(1);
    }
    return value;
}

/// @return @p value without its leading whitespace.
constexpr std::string_view trimLeft(std::string_view value) noexcept
{
    while (!value.empty())
    {
        if (!Whitespace.contains(value.front()))
            return value;
        value.remove_prefix(1);
    }
    return value;
}

/// @return @p value without its leading or trailing whitespace.
constexpr std::string_view trim(std::string_view value) noexcept
{
    return trimLeft(trimRight(value));
}

template <typename T>
constexpr bool ascending(T low, T val, T high) noexcept
{
    return low <= val && val <= high;
}

/// Joins a range's elements into one human-readable, separator-delimited string.
///
/// Takes a range rather than a `std::vector` so a caller can pass a view straight over a table it
/// already holds, instead of materializing one only to name it once. libstdc++ 14 — the oldest
/// standard library this project builds against — is also missing the `std::from_range` container
/// constructors that the materializing spelling reaches for first.
///
/// @param list The elements to join, in order.
/// @param sep Placed between consecutive elements.
/// @return The joined text; empty when the range is.
template <std::ranges::input_range Range>
[[nodiscard]] std::string joinHumanReadable(Range&& list, std::string_view sep = ", ")
{
    auto result = std::string {};
    auto isFirst = true;
    for (auto const& element: std::forward<Range>(list))
    {
        if (!std::exchange(isFirst, false))
            result += sep;
        result += std::format("{}", element);
    }
    return result;
}

/// Joins a range's elements into one separator-delimited string, each element quoted and escaped.
///
/// @param list The elements to join, in order.
/// @param sep Placed between consecutive elements.
/// @return The joined text; empty when the range is.
template <std::ranges::input_range Range, typename Separator>
[[nodiscard]] std::string joinHumanReadableQuoted(Range&& list, Separator sep = ", ")
{
    auto result = std::string {};
    auto isFirst = true;
    for (auto const& element: std::forward<Range>(list))
    {
        if (!std::exchange(isFirst, false))
            result += std::format("{}", sep);
        result += std::format("\"{}\"", crispy::escape(std::format("{}", element)));
    }
    return result;
}

template <typename T, typename Callback>
constexpr bool split(std::basic_string_view<T> text,
                     T delimiter,
                     Callback const& callback) noexcept(noexcept(callback(std::basic_string_view<T> {})))
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
constexpr auto split(std::basic_string_view<T> text, T delimiter) -> std::vector<std::basic_string_view<T>>
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

/// Maps a character to its value as a digit in the given base.
///
/// @param ch   The character to interpret.
/// @tparam Base The numeric base; digits at or above it are rejected. A template parameter rather
///              than an argument because every caller knows it at compile time, and it is what
///              lets the range test fold away instead of being re-decided per character.
/// @return The digit's value, or std::nullopt if @p ch is not a digit in @p Base.
template <std::size_t Base = 16, typename C>
[[nodiscard]] constexpr std::optional<unsigned> digitValue(C ch) noexcept
{
    auto value = std::numeric_limits<unsigned>::max();
    if ('0' <= ch && ch <= '9')
        value = static_cast<unsigned>(ch - '0');
    else if ('a' <= ch && ch <= 'f')
        value = static_cast<unsigned>(ch - 'a') + 10;
    else if ('A' <= ch && ch <= 'F')
        value = static_cast<unsigned>(ch - 'A') + 10;

    // One test covers both "not a digit at all" and "not a digit in this base", and with Base a
    // constant it folds to a single compare.
    if (value < Base)
        return value;
    return std::nullopt;
}

/// Parses @p text as an unsigned numeric literal in the given base.
///
/// Rejects -- rather than wraps or overflows -- any input whose value does not fit in @p T. Much
/// of the input reaching this function is escape-sequence text from the connected program, which
/// is neither length- nor range-bounded; without the check, the signed instantiations are plain
/// undefined behaviour on a long enough digit run.
///
/// @tparam Base The numeric base: 2, 8, 10 or 16.
/// @tparam T    The result type.
/// @param text  The digits to parse. No sign, whitespace or base prefix is accepted.
/// @return The parsed value, or std::nullopt if @p text is empty, holds a non-digit, or names a
///         value too large for @p T.
template <std::size_t Base = 10, typename T = unsigned, typename C>
[[nodiscard]] constexpr std::optional<T> toInteger(std::basic_string_view<C> text) noexcept
{
    static_assert(Base == 2 || Base == 8 || Base == 10 || Base == 16, "Only base-2/8/10/16 supported.");
    static_assert(std::is_integral_v<T>, "T must be an integral type.");
    static_assert(std::is_integral_v<C>, "C must be an integral type.");

    if (text.empty())
        return std::nullopt;

    // Accumulating in the widest unsigned type keeps every intermediate defined regardless of T,
    // so the range check below is a comparison rather than an overflow that already happened.
    // `value * Base + digit <= Limit` is tested as `value < Limit/Base`, or equality on the quotient
    // with the digit against the remainder; both divisions are by constants and fold at compile time.
    auto constexpr Limit = static_cast<std::uintmax_t>(std::numeric_limits<T>::max());
    auto constexpr LimitDiv = Limit / Base;
    auto constexpr LimitMod = Limit % Base;
    auto value = std::uintmax_t { 0 };

    for (auto const ch: text)
    {
        auto const digit = digitValue<Base>(ch);
        if (!digit.has_value())
            return std::nullopt;

        if (value > LimitDiv || (value == LimitDiv && digit.value() > LimitMod))
            return std::nullopt;

        value = (value * Base) + digit.value();
    }

    return static_cast<T>(value);
}

template <std::size_t Base = 10, typename T = unsigned, typename C>
constexpr std::optional<T> toInteger(std::basic_string<C> const& text) noexcept
{
    return toInteger<Base, T, C>(std::basic_string_view<C>(text));
}

class Finally
{
  public:
    explicit Finally(std::function<void()> hook): _hook(std::move(hook)) {}

    Finally(Finally const&) = delete;
    Finally& operator=(Finally const&) = delete;
    Finally(Finally&&) = delete;
    Finally& operator=(Finally&&) = delete;

    void run()
    {
        if (_hook)
        {
            auto hooked = std::move(_hook);
            _hook = {};
            hooked();
        }
    }

    ~Finally() { run(); }

  private:
    std::function<void()> _hook {};
};

#ifdef CRISPY_CONCEPTS_SUPPORTED

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

inline std::string unescapeURL(std::string_view input)
{
    std::string result;
    result.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i)
    {
        if (input[i] == '%' && i + 2 < input.size())
        {
            auto const h1 = crispy::digitValue<16>(input[i + 1]);
            auto const h2 = crispy::digitValue<16>(input[i + 2]);
            if (h1.has_value() && h2.has_value())
            {
                result.push_back(static_cast<char>((h1.value() << 4) | h2.value()));
                i += 2;
                continue;
            }
        }
        result.push_back(input[i]);
    }
    return result;
}

struct ForEachKeyValueParams
{
    std::string_view text;
    char entryDelimiter;
    char assignmentDelimiter;

    template <typename Callback>
        requires std::is_invocable_v<Callback, std::string_view, std::string_view>
    void operator()(Callback const& callback) noexcept(noexcept(callback(std::string_view {},
                                                                         std::string_view {})))
    {
        split(text, entryDelimiter, [&](std::string_view keyValuePair) {
            auto const assignmentSeparator = keyValuePair.find(assignmentDelimiter);
            if (assignmentSeparator != std::string_view::npos)
                callback(keyValuePair.substr(0, assignmentSeparator),
                         keyValuePair.substr(assignmentSeparator + 1));
            else if (!keyValuePair.empty())
                callback(keyValuePair, std::string_view {});
            return true;
        });
    }
};

template <typename Callback>
    requires std::is_invocable_v<Callback, std::string_view, std::string_view>
constexpr void forEachKeyValue(ForEachKeyValueParams params,
                               Callback&& callback) noexcept(noexcept(callback(std::string_view {},
                                                                               std::string_view {})))
{
    params(std::forward<Callback>(callback));
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
        auto const c1 = digitValue<16>(*i++);
        auto const c2 = digitValue<16>(*i++);
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
constexpr auto eachElement() noexcept
{
    struct Container
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
    return Container {};
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

inline std::filesystem::path homeResolvedPath(std::string input, std::filesystem::path const& homeDirectory)
{
    if (!input.empty() && input[0] == '~')
    {
        bool const pathSepFound = input.size() >= 2 && (input[1] == '/' || input[1] == '\\');
        auto subPath = input.substr(pathSepFound ? 2 : 1);
        return homeDirectory / std::filesystem::path(subPath);
    }

    return { std::move(input) };
}

/// Substitutes each `${NAME}` in @p text with `replace(NAME)`.
///
/// A `$${` sequence escapes substitution: it emits a literal `${` and leaves the rest of the
/// would-be variable untouched, so machine-emitted config (e.g. SaveLayout) can round-trip values
/// that contain literal `${...}` text without them being re-expanded on reload.
template <typename VariableReplacer>
inline std::string replaceVariables(std::string_view text, VariableReplacer const& replace)
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

        // "$${" escapes expansion: drop the escaping '$', emit a literal "${", and continue
        // scanning right after it (the variable body then flows through as plain text).
        if (markerStartOffset > i && text[markerStartOffset - 1] == '$')
        {
            output += text.substr(i, markerStartOffset - 1 - i);
            output += MarkerStart;
            i = markerStartOffset + MarkerStart.size();
            continue;
        }

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
constexpr void ignoreUnused(Ts const&... /*values*/) noexcept
{
}

std::string threadName();

template <class... Ts>
struct Overloaded: Ts...
{
    using Ts::operator()...;
};

template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

template <typename T, typename... Ts>
concept oneOf = (std::same_as<T, Ts> || ...);

} // namespace crispy
