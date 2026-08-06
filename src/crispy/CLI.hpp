// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <crispy/Utils.hpp>

#include <algorithm>
#include <format>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace crispy::cli
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

    OptionName(char shortName, std::string_view longName): shortName { shortName }, longName { longName } {}

    OptionName(std::string_view longName): longName { longName } {}
    OptionName(char const* longName): longName { longName } {}

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

class ParserError: public std::runtime_error
{
  public:
    explicit ParserError(std::string const& msg): std::runtime_error(msg) {}
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
 * @returns a @c FlagStore containing the parsed result or std::nullopt on failure.
 */
std::optional<FlagStore> parse(Command const& command, StringViewList const& args);

/**
 * Parses the command line arguments with respect to @p command as passed via (argc, argv) suitable
 * for a general main() functions's argc and argv.
 *
 * @returns a @c FlagStore containing the parsed result or std::nullopt on failure.
 */
std::optional<FlagStore> parse(Command const& command, int argc, char const* const* argv);

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
        using crispy::toLower;
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

} // end namespace crispy::cli

// {{{ type formatters
template <>
struct std::formatter<crispy::cli::Value>
{
    auto parse(format_parse_context& ctx) -> format_parse_context::iterator { return ctx.begin(); }
    auto format(crispy::cli::Value const& value, auto& ctx) const
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
// }}} namespace fmt
