// SPDX-License-Identifier: Apache-2.0
#include <contour/config/DaemonExtensionConfig.hpp>

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <format>
#include <locale>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

using namespace std::string_view_literals;

namespace contour::config
{

namespace
{
    /// How YAML 1.2's core schema spells what is not a number or a string.
    struct PlainWord
    {
        std::string_view spelling; ///< The plain scalar.
        std::string_view json;     ///< Its JSON; empty for a value JSON cannot spell.
    };

    // clang-format off
    inline constexpr auto PlainWords = std::array {
        PlainWord { .spelling = "~", .json = "null" },     PlainWord { .spelling = "null", .json = "null" },
        PlainWord { .spelling = "Null", .json = "null" },  PlainWord { .spelling = "NULL", .json = "null" },
        PlainWord { .spelling = "true", .json = "true" },  PlainWord { .spelling = "True", .json = "true" },
        PlainWord { .spelling = "TRUE", .json = "true" },  PlainWord { .spelling = "false", .json = "false" },
        PlainWord { .spelling = "False", .json = "false" }, PlainWord { .spelling = "FALSE", .json = "false" },
        PlainWord { .spelling = ".nan", .json = "" },      PlainWord { .spelling = ".NaN", .json = "" },
        PlainWord { .spelling = ".NAN", .json = "" },
    };
    // clang-format on

    [[nodiscard]] constexpr bool isDigit(char c, int base) noexcept
    {
        switch (base)
        {
            case 8: return c >= '0' && c <= '7';
            case 16: return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
            default: return c >= '0' && c <= '9';
        }
    }

    /// @return The number of @p base digits @p text starts with.
    [[nodiscard]] constexpr std::size_t digitsOf(std::string_view text, int base = 10) noexcept
    {
        return static_cast<std::size_t>(
            std::ranges::find_if_not(text, [base](char c) { return isDigit(c, base); }) - text.begin());
    }

    /// @return @p text without one leading sign.
    [[nodiscard]] constexpr std::string_view withoutSign(std::string_view text) noexcept
    {
        return text.starts_with('+') || text.starts_with('-') ? text.substr(1) : text;
    }

    /// @return Whether @p text is the core schema's float:
    /// `[-+]?(\.[0-9]+|[0-9]+(\.[0-9]*)?)([eE][-+]?[0-9]+)?`.
    [[nodiscard]] constexpr bool isFloat(std::string_view text) noexcept
    {
        auto rest = withoutSign(text);
        auto const whole = digitsOf(rest);
        rest.remove_prefix(whole);
        if (rest.starts_with('.'))
        {
            auto const fraction = digitsOf(rest.substr(1));
            if (whole == 0 && fraction == 0)
                return false;
            rest.remove_prefix(1 + fraction);
        }
        else if (whole == 0)
            return false;
        if (rest.starts_with('e') || rest.starts_with('E'))
        {
            rest = withoutSign(rest.substr(1));
            auto const exponent = digitsOf(rest);
            if (exponent == 0)
                return false;
            rest.remove_prefix(exponent);
        }
        return rest.empty();
    }

    /// The sign a number was written with.
    enum class Sign : uint8_t
    {
        Positive,
        Negative,
    };

    /// @return The sign @p text starts with; Positive for none.
    [[nodiscard]] constexpr Sign signOf(std::string_view text) noexcept
    {
        return text.starts_with('-') ? Sign::Negative : Sign::Positive;
    }

    /// @return Whether @p text, its sign removed, spells an infinity, which JSON cannot.
    [[nodiscard]] constexpr bool isInfinity(std::string_view text) noexcept
    {
        return text == ".inf" || text == ".Inf" || text == ".INF";
    }

    /// @return The integer @p digits spell in @p base, with @p sign, as JSON; NotRepresentable past 64 bits.
    [[nodiscard]] std::expected<std::string, YamlToJsonError> integerJson(std::string_view digits,
                                                                          int base,
                                                                          Sign sign)
    {
        auto magnitude = uint64_t {};
        auto const [end, error] =
            std::from_chars(digits.data(), digits.data() + digits.size(), magnitude, base);
        if (error != std::errc {} || end != digits.data() + digits.size())
            return std::unexpected(YamlToJsonError::NotRepresentable);
        if (sign == Sign::Positive)
            return std::format("{}", magnitude);
        if (magnitude > uint64_t { 1 } << 63u)
            return std::unexpected(YamlToJsonError::NotRepresentable);
        return std::format("-{}", magnitude);
    }

    /// @return The double all of @p text spells, read in the classic locale; nothing when it does not.
    /// A stream rather than std::from_chars, whose floating-point overloads libc++ 19 (AppleClang 17's)
    /// lacks: a double target resolves to the deleted bool overload there.
    [[nodiscard]] std::optional<double> doubleOf(std::string_view text)
    {
        auto stream = std::istringstream { std::string(text) };
        stream.imbue(std::locale::classic());
        auto value = 0.0;
        stream >> value;
        if (stream.fail() || stream.peek() != std::istringstream::traits_type::eof())
            return std::nullopt;
        return value;
    }

    /// Appends @p text as a JSON string.
    void appendString(std::string& out, std::string_view text)
    {
        out += '"';
        for (auto const c: text)
        {
            switch (c)
            {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20)
                        out += std::format("\\u{:04x}", static_cast<unsigned>(c));
                    else
                        out += c;
            }
        }
        out += '"';
    }

    /// @return The JSON of a plain scalar, by the core schema: null, bool, int, float, else a string.
    [[nodiscard]] std::expected<std::string, YamlToJsonError> plainScalarJson(std::string_view text)
    {
        if (text.empty())
            return "null";
        if (auto const word = std::ranges::find(PlainWords, text, &PlainWord::spelling);
            word != PlainWords.end())
        {
            if (word->json.empty())
                return std::unexpected(YamlToJsonError::NotRepresentable);
            return std::string(word->json);
        }
        auto const signedText = withoutSign(text);
        if (signedText.size() > 2 && signedText.size() == text.size()
            && (signedText.starts_with("0o") || signedText.starts_with("0x")))
        {
            auto const base = signedText[1] == 'o' ? 8 : 16;
            if (digitsOf(signedText.substr(2), base) == signedText.size() - 2)
                return integerJson(signedText.substr(2), base, Sign::Positive);
        }
        if (!signedText.empty() && digitsOf(signedText) == signedText.size())
            return integerJson(signedText, 10, signOf(text));
        if (isInfinity(signedText))
            return std::unexpected(YamlToJsonError::NotRepresentable);
        if (isFloat(text))
        {
            auto const value = doubleOf(signedText);
            if (!value || !std::isfinite(*value))
                return std::unexpected(YamlToJsonError::NotRepresentable);
            return std::format("{}", signOf(text) == Sign::Negative ? -*value : *value);
        }
        auto json = std::string {};
        appendString(json, text);
        return json;
    }

    /// Appends @p node as JSON to @p out.
    /// @param depth How many lists and maps hold @p node.
    [[nodiscard]] std::expected<void, YamlToJsonError> append(std::string& out,
                                                              YAML::Node const& node,
                                                              std::size_t depth,
                                                              std::size_t maxDepth)
    {
        switch (node.Type())
        {
            case YAML::NodeType::Undefined:
            case YAML::NodeType::Null: out += "null"; return {};
            case YAML::NodeType::Scalar: {
                // A plain scalar's tag is the non-specific `?`; a quoted one's is `!`, and it stays text.
                if (node.Tag() != "?")
                {
                    appendString(out, node.Scalar());
                    return {};
                }
                auto json = plainScalarJson(node.Scalar());
                if (!json)
                    return std::unexpected(json.error());
                out += *json;
                return {};
            }
            case YAML::NodeType::Sequence: {
                if (depth + 1 > maxDepth)
                    return std::unexpected(YamlToJsonError::TooDeep);
                out += '[';
                auto separator = ""sv;
                for (auto const& element: node)
                {
                    out += separator;
                    separator = ","sv;
                    if (auto appended = append(out, element, depth + 1, maxDepth); !appended)
                        return appended;
                }
                out += ']';
                return {};
            }
            case YAML::NodeType::Map: {
                if (depth + 1 > maxDepth)
                    return std::unexpected(YamlToJsonError::TooDeep);
                out += '{';
                auto separator = ""sv;
                auto keys = std::set<std::string, std::less<>> {};
                for (auto const& item: node)
                {
                    if (!item.first.IsScalar() || !keys.insert(item.first.Scalar()).second)
                        return std::unexpected(YamlToJsonError::NotRepresentable);
                    out += separator;
                    separator = ","sv;
                    appendString(out, item.first.Scalar());
                    out += ':';
                    if (auto appended = append(out, item.second, depth + 1, maxDepth); !appended)
                        return appended;
                }
                out += '}';
                return {};
            }
        }
        return std::unexpected(YamlToJsonError::NotRepresentable);
    }
} // namespace

std::expected<std::string, YamlToJsonError> yamlToJson(YAML::Node const& node, std::size_t maxDepth)
{
    auto json = std::string {};
    return append(json, node, 0, maxDepth).transform([&] { return std::move(json); });
}

} // namespace contour::config
