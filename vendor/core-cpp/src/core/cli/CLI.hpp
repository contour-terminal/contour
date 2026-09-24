// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Utils.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace core::cli
{

using Value = std::variant<int, unsigned int, std::string, double, bool>;
using Name = std::string;

enum class Presence : uint8_t
{
    Optional,
    Required,
};

struct OptionName
{
    char shortName {};
    std::string_view longName {};

    OptionName(char shortSpelling, std::string_view longSpelling):
        shortName { shortSpelling }, longName { longSpelling }
    {
    }

    OptionName(std::string_view longSpelling): longName { longSpelling } {}
    OptionName(char const* longSpelling): longName { longSpelling } {}

    OptionName(OptionName const&) = default;
    OptionName(OptionName&&) = default;
    OptionName& operator=(OptionName const&) = default;
    OptionName& operator=(OptionName&&) = default;
    ~OptionName() = default;
};

struct Deprecated
{
    std::string_view message;
};

struct Option
{
    OptionName name;
    Value v;
    std::string_view helpText = {};
    std::string_view placeholder = {}; // TODO: move right below `Value value{};`
    cli::Presence presence = Presence::Optional;
    std::optional<cli::Deprecated> deprecated = std::nullopt;
};

using OptionList = std::vector<Option>;

enum class CommandSelect : uint8_t
{
    Explicit,
    Implicit, // only one command at a scope level can be implicit
};

struct Verbatim
{
    std::string placeholder;
    std::string helpText;
};

struct Command
{
    using CommandList = std::vector<Command>;
    std::string_view name;
    std::string_view helpText = {};
    OptionList options = {};
    // std::vector<Command> children = {};
    CommandList children = {};
    CommandSelect select = CommandSelect::Explicit;
    std::optional<cli::Verbatim> verbatim = {}; // Only allowed if no sub commands were specified.
};

using CommandList = Command::CommandList;

enum class OptionStyle : uint8_t
{
    Natural,
    Posix,
};

/// Why @c parse refused a command line.
enum class ParseErrorKind : std::uint8_t
{
    NotEnoughArguments,    ///< The command line ended where a token was needed: a name or a value.
    InvalidValue,          ///< A value cannot be read as the type its option declares.
    EmptyValue,            ///< `--name=` for an option whose type is not a string.
    UnexpectedToken,       ///< A token that is neither an option, a sub-command nor verbatim input.
    MissingRequiredOption, ///< An option marked @c Presence::Required was not given.
};

/// What was wrong with a command line, and where.
struct ParseError
{
    ParseErrorKind kind = ParseErrorKind::NotEnoughArguments; ///< What was wrong.
    /// The index into the argument list of the token at fault. For @c NotEnoughArguments and
    /// @c MissingRequiredOption, where no token is at fault, the argument count.
    std::size_t tokenIndex = 0;
    std::string message; ///< A sentence for the user, naming the option or token concerned.
};

struct FlagStore
{
    std::map<Name, Value> values;
    std::vector<std::string_view> verbatim;

    [[nodiscard]] bool boolean(std::string const& key) const { return std::get<bool>(values.at(key)); }
    [[nodiscard]] int integer(std::string const& key) const { return std::get<int>(values.at(key)); }
    [[nodiscard]] unsigned uint(std::string const& key) const { return std::get<unsigned>(values.at(key)); }
    [[nodiscard]] double real(std::string const& key) const { return std::get<double>(values.at(key)); }
    [[nodiscard]] std::string const& str(std::string const& key) const
    {
        return std::get<std::string>(values.at(key));
    }

    template <typename T>
    [[nodiscard]] T get(std::string const& key) const
    {
        return std::get<T>(values.at(key));
    }
};

/*
 * Validates @p command to be well-formed and throws an exception otherwise.
 */
// TODO: void validate(Command const& command);

using StringViewList = std::vector<std::string_view>;

/**
 * Parses the command line arguments with respect to @p command as passed via @p args.
 *
 * A malformed command line is a value, never an exception: nothing in the parser throws.
 * Numbers are read with `std::from_chars` (floating point with `std::strtod`), and the whole token
 * must be the number -- `12abc` is refused rather than read as 12, and `-1` is not an unsigned.
 *
 * @param command The syntax to parse against.
 * @param args The arguments; the first is the command's own name (`argv[0]`), and is not checked.
 * @return A @c FlagStore holding every option's value, the defaults included, or the
 *         @c ParseError that says what was wrong and at which token.
 */
[[nodiscard]] std::expected<FlagStore, ParseError> parse(Command const& command, StringViewList const& args);

/**
 * Parses the command line arguments with respect to @p command as passed via (argc, argv) suitable
 * for a general main() functions's argc and argv.
 *
 * @param command The syntax to parse against.
 * @param argc The argument count.
 * @param argv The arguments, `argv[0]` first.
 * @return As for the @c StringViewList overload.
 */
[[nodiscard]] std::expected<FlagStore, ParseError> parse(Command const& command,
                                                         int argc,
                                                         char const* const* argv);

enum class HelpElement : uint8_t
{
    Header,
    Braces,
    OptionDash,
    OptionName,
    OptionEqual,
    OptionValue,
    ImplicitCommand,
    Verbatim,
    HelpText
};

struct HelpDisplayStyle
{
    using ColorMap = std::map<HelpElement, std::string>;
    static ColorMap defaultColors();

    std::optional<ColorMap> colors = defaultColors();
    bool hyperlink = true; // whether or not to enable OSC 8 (Hyperlink).
    OptionStyle optionStyle = OptionStyle::Natural;
};

/**
 * Constructs a usage text suitable for printing out the command usage syntax in terminals.
 *
 * @param command      The command to construct the usage text for.
 * @param style        Determines how to format and colorize the output string.
 * @param margin       Number of characters to write at most per line.
 * @param cmdPrefix    Some text to prepend in front of each generated line in the output.
 */
std::string usageText(Command const& command,
                      HelpDisplayStyle const& style,
                      unsigned margin,
                      std::string const& cmdPrefix = {});

/**
 * Constructs a help text suitable for printing out the command usage syntax in terminals.
 *
 * @param command      The command to construct the usage text for.
 * @param style        Determines how to format and colorize the output string.
 * @param margin       Number of characters to write at most per line.
 */
std::string helpText(Command const& command, HelpDisplayStyle const& style, unsigned margin);

// Throw if command is not well defined.
void validate(Command const& command);

namespace about
{
    struct Project
    {
        std::string_view title;
        std::string_view license;
        std::string_view url;
    };

    inline std::vector<Project>& store()
    {
        static std::vector<Project> instance;
        return instance;
    }

    inline void registerProjects(Project project)
    {
        store().emplace_back(project);
        using core::toLower;
        std::sort(store().begin(), store().end(), [](auto const& a, auto const& b) {
            return toLower(a.title) < toLower(b.title);
        });
    }

    template <typename... Args>
    void registerProjects(Project project0, Args... more)
    {
        store().emplace_back(project0);
        registerProjects(more...);
    }
} // namespace about

} // end namespace core::cli

// {{{ type formatters
template <>
struct std::formatter<core::cli::Value>
{
    auto parse(format_parse_context& ctx) -> format_parse_context::iterator { return ctx.begin(); }
    auto format(core::cli::Value const& value, auto& ctx) const
    {
        if (std::holds_alternative<bool>(value))
            return std::format_to(ctx.out(), "{}", std::get<bool>(value));
        else if (std::holds_alternative<int>(value))
            return std::format_to(ctx.out(), "{}", std::get<int>(value));
        else if (std::holds_alternative<unsigned>(value))
            return std::format_to(ctx.out(), "{}", std::get<unsigned>(value));
        else if (std::holds_alternative<double>(value))
            return std::format_to(ctx.out(), "{}", std::get<double>(value));
        else if (std::holds_alternative<std::string>(value))
            return std::format_to(ctx.out(), "{}", std::get<std::string>(value));
        else
            return std::format_to(ctx.out(), "?");
        // return std::format_to(ctx.out(), "{}..{}", range.from, range.to);
    }
};
// }}}
