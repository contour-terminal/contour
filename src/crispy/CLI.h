// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <crispy/utils.h>

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

using value = std::variant<int, unsigned int, std::string, double, bool>;
using name = std::string;

enum class presence : uint8_t
{
    Optional,
    Required,
};

struct option_name
{
    char shortName {};
    std::string_view longName {};

    option_name(char shortName, std::string_view longName): shortName { shortName }, longName { longName } {}

    option_name(std::string_view longName): longName { longName } {}
    option_name(char const* longName): longName { longName } {}

    option_name(option_name const&) = default;
    option_name(option_name&&) = default;
    option_name& operator=(option_name const&) = default;
    option_name& operator=(option_name&&) = default;
    ~option_name() = default;
};

struct deprecated
{
    std::string_view message;
};

struct option
{
    option_name name;
    value v;
    std::string_view helpText = {};
    std::string_view placeholder = {}; // TODO: move right below `Value value{};`
    cli::presence presence = presence::Optional;
    std::optional<cli::deprecated> deprecated = std::nullopt;
};

using option_list = std::vector<option>;

enum class command_select : uint8_t
{
    Explicit,
    Implicit, // only one command at a scope level can be implicit
};

struct verbatim
{
    std::string placeholder;
    std::string helpText;
};

struct command
{
    using command_list = std::vector<command>;
    std::string_view name;
    std::string_view helpText = {};
    option_list options = {};
    // std::vector<command> children = {};
    command_list children = {};
    command_select select = command_select::Explicit;
    std::optional<cli::verbatim> verbatim = {}; // Unly allowed if no sub commands were specified.
};

using command_list = command::command_list;

enum class option_style : uint8_t
{
    Natural,
    Posix,
};

class parser_error: public std::runtime_error
{
  public:
    explicit parser_error(std::string const& msg): std::runtime_error(msg) {}
};

struct flag_store
{
    std::map<name, value> values;
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
// TODO: void validate(command const& command);

using string_view_list = std::vector<std::string_view>;

/**
 * Parses the command line arguments with respect to @p command as passed via @p args.
 *
 * @returns a @c FlagStore containing the parsed result or std::nullopt on failure.
 */
std::optional<flag_store> parse(command const& command, string_view_list const& args);

/**
 * Parses the command line arguments with respect to @p command as passed via (argc, argv) suitable
 * for a general main() functions's argc and argv.
 *
 * @returns a @c FlagStore containing the parsed result or std::nullopt on failure.
 */
std::optional<flag_store> parse(command const& command, int argc, char const* const* argv);

enum class help_element : uint8_t
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

struct help_display_style
{
    using color_map = std::map<help_element, std::string>;
    static color_map defaultColors();

    std::optional<color_map> colors = defaultColors();
    bool hyperlink = true; // whether or not to enable OSC 8 (Hyperlink).
    option_style optionStyle = option_style::Natural;
};

/**
 * Constructs a usage text suitable for printing out the command usage syntax in terminals.
 *
 * @param command      The command to construct the usage text for.
 * @param style        Determines how to format and colorize the output string.
 * @param margin       Number of characters to write at most per line.
 * @param cmdPrefix    Some text to prepend in front of each generated line in the output.
 */
std::string usageText(command const& command,
                      help_display_style const& style,
                      unsigned margin,
                      std::string const& cmdPrefix = {});

/**
 * Constructs a help text suitable for printing out the command usage syntax in terminals.
 *
 * @param command      The command to construct the usage text for.
 * @param style        Determines how to format and colorize the output string.
 * @param margin       Number of characters to write at most per line.
 */
std::string helpText(command const& command, help_display_style const& style, unsigned margin);

// Throw if command is not well defined.
void validate(command const& command);

namespace about
{
    struct project
    {
        std::string_view title;
        std::string_view license;
        std::string_view url;
    };

    inline std::vector<project>& store()
    {
        static std::vector<project> instance;
        return instance;
    }

    inline void registerProjects(project project)
    {
        store().emplace_back(project);
        using crispy::toLower;
        std::sort(store().begin(), store().end(), [](auto const& a, auto const& b) {
            return toLower(a.title) < toLower(b.title);
        });
    }

    template <typename... Args>
    void registerProjects(project project0, Args... more)
    {
        store().emplace_back(project0);
        registerProjects(more...);
    }
} // namespace about

} // end namespace crispy::cli

// {{{ type formatters
template <>
struct std::formatter<crispy::cli::value>
{
    auto parse(format_parse_context& ctx) -> format_parse_context::iterator { return ctx.begin(); }
    auto format(crispy::cli::value const& value, auto& ctx) const
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
