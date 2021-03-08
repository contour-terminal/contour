/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <fmt/format.h>

#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace crispy::cli
{

using Value = std::variant<int, unsigned int, std::string, double, bool>;
using Name = std::string;

enum class Presence
{
    Optional,
    Required,
};

struct Option
{
    std::string_view name;
    Value value;
    std::string_view helpText = {};
    std::string_view placeholder = {}; // TODO: move right below `Value value{};`
    Presence presence = Presence::Optional;
};

using OptionList = std::vector<Option>;

enum class CommandSelect
{
    Explicit,
    Implicit, // only one command at a scope level can be implicit
};

struct Verbatim
{
    std::string placeholder;
    std::string helpText;
};

struct Command;
using CommandList = std::vector<Command>;

struct Command
{
    std::string_view name;
    std::string_view helpText = {};
    OptionList options = {};
    //std::vector<Command> children = {};
    CommandList children = {};
    CommandSelect select = CommandSelect::Explicit;
    std::optional<Verbatim> verbatim = {}; // Unly allowed if no sub commands were specified.
};

using CommandList = std::vector<Command>;

enum class OptionStyle
{
    Natural,
    Posix,
};

class ParserError : public std::runtime_error
{
   public:
    explicit ParserError(std::string const& _msg) : std::runtime_error(_msg) {}
};

struct FlagStore
{
    std::map<Name, Value> values;
    std::vector<std::string_view> verbatim;

    bool boolean(std::string const& _key) const { return std::get<bool>(values.at(_key)); }
    int integer(std::string const& _key) const { return std::get<int>(values.at(_key)); }
    unsigned uint(std::string const& _key) const { return std::get<unsigned>(values.at(_key)); }
    double real(std::string const& _key) const { return std::get<double>(values.at(_key)); }
    std::string const& str(std::string const& _key) const { return std::get<std::string>(values.at(_key)); }

    template <typename T> T get(std::string const& _key) const { return std::get<T>(values.at(_key)); }
};

/*
 * Validates @p _command to be well-formed and throws an exception otherwise.
 */
// TODO: void validate(Command const& _command);

using StringViewList = std::vector<std::string_view>;

/**
 * Parses the command line arguments with respect to @p _command as passed via @p _args.
 *
 * @returns a @c FlagStore containing the parsed result or std::nullopt on failure.
 */
std::optional<FlagStore> parse(Command const& _command, StringViewList const& _args);

/**
 * Parses the command line arguments with respect to @p _command as passed via (_argc, _argv) suitable
 * for a general main() functions's argc and argv.
 *
 * @returns a @c FlagStore containing the parsed result or std::nullopt on failure.
 */
std::optional<FlagStore> parse(Command const& _command, int _argc, char const* const* _argv);

enum class HelpElement
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

struct HelpStyle // TODO: maybe call HelpDisplayStyle?
{
    using ColorMap = std::map<HelpElement, std::string>;
    static ColorMap defaultColors();

    std::optional<ColorMap> colors = defaultColors();
    bool hyperlink = true;   // whether or not to enable OSC 8 (Hyperlink).
    OptionStyle optionStyle = OptionStyle::Natural;
};

/**
 * Constructs a usage text suitable for printing out the command usage syntax in terminals.
 *
 * @param _command      The command to construct the usage text for.
 * @param _style        Determines how to format and colorize the output string.
 * @param _margin       Number of characters to write at most per line.
 * @param _cmdPrefix    Some text to prepend in front of each generated line in the output.
 */
std::string usageText(Command const& _command, HelpStyle const& _style, int _margin,
                      std::string const& _cmdPrefix = {});

/**
 * Constructs a help text suitable for printing out the command usage syntax in terminals.
 *
 * @param _command      The command to construct the usage text for.
 * @param _style        Determines how to format and colorize the output string.
 * @param _margin       Number of characters to write at most per line.
 */
std::string helpText(Command const& _command, HelpStyle const& _style, int _margin);

} // end namespace crispy::cli

namespace fmt // {{{ type formatters
{
    template <>
    struct formatter<crispy::cli::Value> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(crispy::cli::Value const& _value, FormatContext& ctx)
        {
            if (std::holds_alternative<bool>(_value))
                return format_to(ctx.out(), "{}", std::get<bool>(_value));
            else if (std::holds_alternative<int>(_value))
                return format_to(ctx.out(), "{}", std::get<int>(_value));
            else if (std::holds_alternative<unsigned>(_value))
                return format_to(ctx.out(), "{}", std::get<unsigned>(_value));
            else if (std::holds_alternative<double>(_value))
                return format_to(ctx.out(), "{}", std::get<double>(_value));
            else if (std::holds_alternative<std::string>(_value))
                return format_to(ctx.out(), "{}", std::get<std::string>(_value));
            else
                return format_to(ctx.out(), "?");
            //return format_to(ctx.out(), "{}..{}", range.from, range.to);
        }
    };
} // }}}
