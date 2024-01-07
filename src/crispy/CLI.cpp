// SPDX-License-Identifier: Apache-2.0
#include <crispy/CLI.h>
#include <crispy/assert.h>
#include <crispy/logstore.h>
#include <crispy/times.h>

#include <range/v3/view/enumerate.hpp>
#include <range/v3/view/iota.hpp>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <deque>
#include <sstream>

#if 0 // !defined(NDEBUG)
    #include <iostream>
    #define CLI_DEBUG(that)                   \
        do                                    \
        {                                     \
            std::cerr << (that) << std::endl; \
        } while (0)
#else
    #define CLI_DEBUG(that) \
        do                  \
        {                   \
        } while (0)
#endif

/*
    Grammar
    =======

        CLI     := Command
        Command := NAME Option* SubCommand?
        Option  := NAME [Value]
        SubCommand := Command

        Value   := STR | BOOL | FLOAT | INT | UINT
        NAME    := <name without = or leading -'s>

    Examples
    ========

        # POSIX style
        contour --debug '*' capture --logical --timeout=1.0 --output="file.vt"
        contour --debug '*' capture -l -t 1.0 -o "file.vt"

        capture --config="contour.yml" --debug="foo,bar,com.*"

        # NATURAL STYLE
        contour debug '*' capture logical timeout 1.0 output "file.vt"
        capture config "contour.yml" debug "foo,bar,com.*"

        # MIXED STYLE
        contour -d '*' capture logical timeout 1.0 output "file.vt"
*/

using std::deque;
using std::function;
using std::get;
using std::holds_alternative;
using std::invalid_argument;
using std::map;
using std::max;
using std::move;
using std::nullopt;
using std::optional;
using std::ostream;
using std::pair;
using std::stod;
using std::stoi;
using std::stoul;
using std::string;
using std::string_view;
using std::stringstream;
using std::vector;

using namespace std::string_view_literals;
using namespace std::string_literals;

namespace crispy::cli
{

namespace // {{{ helper
{
    struct parse_context
    {
        string_view_list const& args;
        size_t pos = 0;

        deque<command const*> currentCommand = {};
        option const* currentOption = nullptr;

        flag_store output = {};
    };

    auto namePrefix(parse_context const& context, char delim = '.') -> string // {{{
    {
        string output;
        for (auto const&& [i, cmd]: ranges::views::enumerate(context.currentCommand))
        {
            command const* v = context.currentCommand.at(i);
            if (i != 0)
                output += delim;
            output += v->name;
        }

        return output;
    } //  }}}

    bool hasTokensAvailable(parse_context const& context)
    {
        return context.pos < context.args.size();
    }

    auto currentToken(parse_context const& context) -> string_view
    {
        if (context.pos >= context.args.size())
            return string_view {}; // not enough arguments available

        return context.args.at(context.pos);
    }

    auto isTrue(string_view token) -> bool
    {
        return token == "true" || token == "yes";
    }

    auto isFalse(string_view token) -> bool
    {
        return token == "false" || token == "no";
    }

    bool matchPrefix(string_view text, string_view prefix)
    {
        if (text.size() < prefix.size())
            return false;
        for (size_t i = 0; i < prefix.size(); ++i)
            if (text[i] != prefix[i])
                return false;
        return true;
    }

    option const* findOption(parse_context const& context, string_view name)
    {
        for (auto const& option: context.currentCommand.back()->options)
            if (name == option.name.longName || (name.size() == 1 && name[0] == option.name.shortName))
            {
                if (option.deprecated)
                    errorLog()("Deprecated option \"{}\" used. {}",
                               option.name.longName,
                               option.deprecated.value().message);
                return &option;
            }
        return nullptr;
    }

    string_view consumeToken(parse_context& context)
    {
        // NAME := <just a name>
        if (context.pos >= context.args.size())
            throw parser_error("Not enough arguments specified.");

        CLI_DEBUG(fmt::format("Consuming token '{}'", currentToken(context)));
        return context.args.at(context.pos++);
    }

    /// Parses the given parameter value @p text with respect to the given @p option.
    value parseValue(parse_context& context, string_view text) // {{{
    {
        // Value := STR | BOOL | FLOAT | INT | UINT

        // BOOL
        if (holds_alternative<bool>(context.currentOption->v))
        {
            if (isTrue(text))
                return value { true };

            if (isFalse(text))
                return value { false };

            throw parser_error("Boolean value expected but something else specified.");
        }

        // FLOAT
        try
        {
            if (holds_alternative<double>(context.currentOption->v))
            {
                return value { stod(string(text)) }; // TODO: avoid malloc
            }
        }
        catch (...)
        {
            throw parser_error("Floating point value expected but something else specified.");
        }

        // UINT
        try
        {
            if (holds_alternative<unsigned>(context.currentOption->v))
                return value { unsigned(stoul(string(text))) };
        }
        catch (...)
        {
            throw parser_error("Unsigned integer value expected but something else specified.");
        }

        // INT
        try
        {
            if (holds_alternative<int>(context.currentOption->v))
                return value { stoi(string(text)) };
        }
        catch (...)
        {
            throw parser_error("Integer value expected but something else specified.");
        }

        // STR
        return value { string(text) };
    }                                        // }}}
    value parseValue(parse_context& context) // {{{
    {
        if (holds_alternative<bool>(context.currentOption->v))
        {
            auto const text = currentToken(context);
            if (isTrue(text))
            {
                consumeToken(context);
                return value { true };
            }

            if (isFalse(text))
            {
                consumeToken(context);
                return value { false };
            }

            // Booleans can be specified just by `--flag` or `flag` without any value
            // and are considered to be true (implicit).
            return value { true };
        }
        return parseValue(context, consumeToken(context));
    } // }}}

    struct scoped_option
    {
        parse_context& context;
        scoped_option(parse_context& context, option const& option): context { context }
        {
            context.currentOption = &option;
        }
        ~scoped_option() { context.currentOption = nullptr; }
    };

    struct scoped_command
    {
        parse_context& context;
        scoped_command(parse_context& context, command const& command): context { context }
        {
            context.currentCommand.emplace_back(&command);
        }
        ~scoped_command() { context.currentCommand.pop_back(); }
    };

    /// Tries parsing an option name and, if matching, also its value if provided.
    ///
    /// @throw ParserError on parserfailures
    /// @returns nullptr if current token is no option name a pair of an @c Option pointer and its optional
    /// value otherwise.
    optional<pair<option const*, value>> tryParseOption(parse_context& context)
    {
        // NAME [VALUE]
        // -NAME [VALUE]
        // --NAME[=VALUE]
        auto const current = currentToken(context);
        if (matchPrefix(current, "--")) // POSIX-style long-option
        {
            if (auto const i = current.find('='); i != std::string_view::npos)
            {
                auto const name = current.substr(2, i - 2);
                auto const valueText = current.substr(i + 1);
                if (option const* opt = findOption(context, name)) // --NAME=VALUE
                {
                    consumeToken(context);
                    if (valueText.empty() && !holds_alternative<string>(opt->v))
                        throw parser_error("Explicit empty value passed but a non-string value expected.");

                    auto const optionScope = scoped_option { context, *opt };
                    return pair { opt, parseValue(context, valueText) };
                }
            }
            else
            {
                auto const name = current.substr(2);
                if (option const* opt = findOption(context, name)) // --NAME
                {
                    consumeToken(context);
                    auto const optionScope = scoped_option { context, *opt };
                    return pair { opt, parseValue(context) };
                }
            }
        }
        else if (matchPrefix(current, "-")) // POSIX-style short opt (or otherwise ...)
        {
            auto const name = current.substr(1);
            if (option const* opt = findOption(context, name)) // -NAME
            {
                consumeToken(context);
                auto const optionScope = scoped_option { context, *opt };
                return pair { opt, parseValue(context) };
            }
        }
        else // Natural style option
        {
            auto const name = current;
            if (option const* opt = findOption(context, name)) // -NAME
            {
                consumeToken(context);
                auto const optionScope = scoped_option { context, *opt };
                return pair { opt, parseValue(context) };
            }
        }

        return nullopt;
    }

    void setOption(parse_context& context, string const& key, value value)
    {
        CLI_DEBUG(fmt::format("setOption({}): {}", key, value));
        context.output.values[key] = std::move(value);
    }

    void parseOptionList(parse_context& context)
    {
        // Option := Option*
        auto const optionPrefix = namePrefix(context);

        while (true)
        {
            auto optionOptPair = tryParseOption(context);
            if (!optionOptPair.has_value())
                break;

            auto& [option, value] = optionOptPair.value();
            auto const fqdn = optionPrefix + "." + name(option->name.longName);
            setOption(context, fqdn, std::move(value));
        }
    }

    auto tryLookupCommand(parse_context const& context) -> command const*
    {
        auto const token = matchPrefix(currentToken(context), "--") ? currentToken(context).substr(2)
                                                                    : currentToken(context);

        for (command const& command: context.currentCommand.back()->children)
        {
            if (token == command.name)
                return &command;
        }

        return nullptr; // not found
    }

    auto tryImplicitCommand(parse_context const& context) -> command const*
    {
        for (command const& command: context.currentCommand.back()->children)
            if (command.select == command_select::Implicit)
            {
                CLI_DEBUG(fmt::format("Select implicit command {}.", command.name));
                return &command;
            }

        return nullptr;
    }

    void prefillDefaults(parse_context& context, command const& com)
    {
        auto const commandScope = scoped_command { context, com };
        auto const prefix = namePrefix(context) + ".";

        for (option const& option: context.currentCommand.back()->options)
        {
            if (option.presence == presence::Required)
                continue; // Do not prefill options that are required anyways.

            auto const fqdn = prefix + name(option.name.longName);
            context.output.values[fqdn] = option.v;
            setOption(context, fqdn, option.v);
        }

        for (command const& subcmd: com.children)
        {
            auto const fqdn = prefix + name(subcmd.name);
            setOption(context, fqdn, value { false });

            prefillDefaults(context, subcmd);
        }
    }

    auto parseCommand(command const& com, parse_context& context) -> bool
    {
        // command := NAME Option* Section*
        auto const commandScope = scoped_command { context, com };
        context.output.values[namePrefix(context)] = value { true };

        parseOptionList(context);

        if (command const* subcmd = tryLookupCommand(context))
        {
            CLI_DEBUG(fmt::format("parseCommand: found sub command: {}", subcmd->name));
            consumeToken(context); // Name was already ensured to be right (or is assumed to be right).
            parseCommand(*subcmd, context);
        }
        else if (command const* subcmd = tryImplicitCommand(context))
        {
            CLI_DEBUG(fmt::format("parseCommand: found implicit sub command: {}", subcmd->name));
            // DO not consume token
            parseCommand(*subcmd, context);
        }
        else if (com.verbatim.has_value())
        {
            CLI_DEBUG(fmt::format("parseCommand: going verbatim."));
            if (hasTokensAvailable(context))
            {
                if (currentToken(context) == "--")
                    (void) consumeToken(context); // consume "--"
                while (context.pos < context.args.size())
                    context.output.verbatim.emplace_back(consumeToken(context));
            }
        }

        if (context.pos == context.args.size())
            context.output.values[namePrefix(context)] = value { true };

        // A command must not leave any trailing tokens at the end of parsing
        return context.pos == context.args.size();
    }

    string_view_list stringViewList(int argc, char const* const* argv)
    {
        string_view_list output;
        output.resize(static_cast<unsigned>(argc));

        for (auto const i: ranges::views::iota(0u, static_cast<unsigned>(argc)))
            output[i] = argv[i];

        return output;
    }

    void validate(command const& com, parse_context& context, string const& keyPrefix)
    {
        auto const key = keyPrefix.empty() ? string(com.name) : fmt::format("{}.{}", keyPrefix, com.name);

        // Ensure all required fields are provided for those commands that have been provided.
        for (option const& option: com.options)
        {
            auto const optionKey = fmt::format("{}.{}", key, option.name.longName);
            // NOLINTNEXTLINE(readability-container-contains)
            if (option.presence == presence::Required && !context.output.values.count(optionKey))
                throw invalid_argument(fmt::format("Missing option: {}", optionKey));
        }

        for (command const& subcmd: com.children)
        {
            auto const commandKey = fmt::format("{}.{}", key, subcmd.name);
            if (context.output.get<bool>(commandKey))
                validate(subcmd, context, key);
        }
    }

} // namespace
// }}}

void validate(command const& command)
{
    (void) command;
    // TODO: throw if command is not well defined.
    //
    // - no duplicated nems in same scope
    // - names must not start with '-' (dash)
    // - must not contain '='
}

optional<flag_store> parse(command const& command, string_view_list const& args)
{
    validate(command);

    auto context = parse_context { args };

    prefillDefaults(context, command);

    // XXX do not enforce checking the first token, as for main()'s argv[0] this most likely is different.
    // if (currentToken(context) != command.name)
    //     return nullopt;

    consumeToken(context); // Name was already ensured to be right (or is assumed to be right).
    if (!parseCommand(command, context))
        return nullopt;

    // auto const& flags = context.output;
    // std::cout << fmt::format("Flags: {}\n", flags.values.size());
    // for (auto const & [k, v] : flags.values)
    //     std::cout << fmt::format(" - {}: {}\n", k, v);

    validate(command, context, "");

    return std::move(context.output);
}

optional<flag_store> parse(command const& command, int argc, char const* const* argv)
{
    return parse(command, stringViewList(argc, argv));
}

} // namespace crispy::cli

namespace crispy::cli
{

namespace // {{{ helpers
{
    string spaces(size_t count)
    {
        return string(count, ' ');
    }

    string indent(unsigned level, unsigned* cursor = nullptr)
    {
        auto constexpr TabWidth = 4u;

        if (cursor != nullptr)
            *cursor += level * TabWidth;

        return spaces(static_cast<size_t>(level) * TabWidth);
    }

    // TODO: this and OSC-8 (hyperlinks)
    auto stylizer(help_display_style const& style) -> function<string(string_view, help_element)>
    {
        return [style](string_view text, help_element element) -> string {
            auto const [pre, post] = [&]() -> pair<string_view, string_view> {
                // NOLINTNEXTLINE(readability-container-contains)
                if (style.colors.has_value() && style.colors.value().count(element))
                    return { style.colors.value().at(element), "\033[m"sv };
                else
                    return { ""sv, ""sv };
            }();

            if (!style.hyperlink)
                return fmt::format("{}{}{}", pre, text, post);

            string output;
            size_t a = 0;
            for (;;)
            {
                size_t const b = text.find("://", a);
                if (b == std::string_view::npos || (b == 0))
                    break;

                size_t left = b;
                while (left > 0 && isalpha(text.at(left - 1))) // NOLINT(readability-implicit-bool-conversion)
                    --left;

                size_t right = b + 3;
                while (right < text.size() && text.at(right) != ' ')
                    right++;

                output += pre;
                output += text.substr(a, left - a);
                output += post;

                output += "\033]8;;";
                output += text.substr(left, right - left);
                output += "\033\\";

                output += text.substr(left, right - left);

                output += "\033]8;;\033\\";

                a = right;
            }
            output += pre;
            output += text.substr(a);
            output += post;

            return output;
        };
    }

    auto colorizer(optional<help_display_style::color_map> const& colors)
        -> function<string(string_view, help_element)>
    {
        help_display_style style {};
        style.colors = colors;
        return stylizer(style);
    }

    string_view wordWrapped(string_view text, unsigned margin, unsigned cursor, bool* trimLeadingWhitespaces)
    {
        auto const linefeed = text.find('\n');
        if (linefeed != string_view::npos)
        {
            auto i = linefeed - 1; // Position before the found LF and trim off right whitespaces.
            while (i > 0 && text[i] == ' ')
                --i;
            *trimLeadingWhitespaces = false;
            return text.substr(0, i + 1);
        }

        *trimLeadingWhitespaces = true;

        auto const unwrappedLength = cursor + text.size();
        if (unwrappedLength <= margin)
            return text;

        // Cut string at right margin, then shift left until we've hit a whitespace character.
        auto const rightMargin = margin - cursor + 1;
        if (rightMargin <= 0)
            return text;

        auto i = rightMargin - 1;
        while (i > 0 && (text[i] != ' ' && text[i] != '\n'))
            --i;

        return text.substr(0, i);
    }

    string wordWrapped(string_view text, unsigned indent, unsigned margin, unsigned* cursor)
    {
        string output;
        size_t i = 0;
        bool trimLeadingWhitespaces = true;
        for (;;)
        {
            auto const trimChar = trimLeadingWhitespaces ? ' ' : '\n';
            while (i < text.size() && text[i] == trimChar)
                ++i; // skip leading whitespaces

            auto const chunk = wordWrapped(text.substr(i), margin, *cursor, &trimLeadingWhitespaces);

            output += chunk;
            *cursor += static_cast<unsigned>(chunk.size());
            i += chunk.size();

            if (i == text.size())
                break;

            output += '\n';
            output += spaces(indent);
            *cursor = indent + 1;
        }
        return output;
    }

    string printParam(optional<help_display_style::color_map> const& colors,
                      option_style optionStyle,
                      option_name const& name,
                      string_view placeholder,
                      presence presense)
    {
        auto const colorize = colorizer(colors);

        stringstream os;

        if (presense == presence::Optional)
            os << colorize("[", help_element::Braces);
        switch (optionStyle)
        {
            case option_style::Natural:
                // if (name.shortName)
                // {
                //     os << colorize(string(1, name.shortName), HelpElement::OptionName);
                //     os << ", ";
                // }
                os << colorize(name.longName, help_element::OptionName);
                if (!placeholder.empty())
                    os << ' ' << colorize(placeholder, help_element::OptionValue);
                break;
            case option_style::Posix:
                if (name.shortName != '\0')
                {
                    os << colorize("-", help_element::OptionDash);
                    os << colorize(string(1, name.shortName), help_element::OptionName);
                    os << ", ";
                }
                os << colorize("--", help_element::OptionDash)
                   << colorize(name.longName, help_element::OptionName);
                if (!placeholder.empty())
                    os << colorize("=", help_element::OptionEqual)
                       << colorize(placeholder, help_element::OptionValue);
                break;
        }
        if (presense == presence::Optional)
            os << colorize("]", help_element::Braces);

        return os.str();
    }

    string printOption(option const& option,
                       optional<help_display_style::color_map> const& colors,
                       option_style optionStyle)
    {
        // TODO: make use of option.placeholder
        auto const placeholder = [](struct option const& option, string_view type) -> string_view {
            return !option.placeholder.empty() ? option.placeholder : type;
        };

        if (holds_alternative<bool>(option.v))
            return printParam(colors, optionStyle, option.name, placeholder(option, ""), option.presence);
        else if (holds_alternative<int>(option.v))
            return printParam(colors, optionStyle, option.name, placeholder(option, "INT"), option.presence);
        else if (holds_alternative<unsigned int>(option.v))
            return printParam(colors, optionStyle, option.name, placeholder(option, "UINT"), option.presence);
        else if (holds_alternative<double>(option.v))
            return printParam(
                colors, optionStyle, option.name, placeholder(option, "FLOAT"), option.presence);
        else
            return printParam(
                colors, optionStyle, option.name, placeholder(option, "STRING"), option.presence);
    }

    string printOption(option const& option,
                       optional<help_display_style::color_map> const& colors,
                       option_style displayStyle,
                       unsigned indent,
                       unsigned margin,
                       unsigned* cursor)
    {
        auto const plainTextLength = static_cast<unsigned>(printOption(option, nullopt, displayStyle).size());
        if (*cursor + plainTextLength < margin)
        {
            *cursor += plainTextLength;
            return printOption(option, colors, displayStyle);
        }
        else
        {
            *cursor = static_cast<unsigned>(indent + 1 + plainTextLength);
            return "\n" + spaces(indent) + printOption(option, colors, displayStyle);
        }
    }

    size_t longestOptionText(option_list const& options, option_style displayStyle)
    {
        size_t result = 0;
        for (option const& option: options)
            result = max(result, printOption(option, nullopt, displayStyle).size());
        return result;
    }

    void detailedDescription(ostream& os,
                             command const& com,
                             help_display_style const& style,
                             unsigned margin,
                             vector<command const*>& parents)
    {
        // NOTE: We asume that cursor position is at first column!
        auto const stylize = stylizer(style);
        bool const hasParentCommand = !parents.empty();
        bool const isLeafCommand = com.children.empty();

        if (isLeafCommand || !com.options.empty() || com.verbatim.has_value()) // {{{ print command sequence
        {
            os << indent(1);
            for (command const* parent: parents)
                os << stylize(parent->name, help_element::OptionValue /*well, yeah*/) << ' ';

            if (com.select == command_select::Explicit)
                os << com.name;
            else
            {
                os << stylize("[", help_element::Braces);
                os << stylize(com.name, help_element::ImplicitCommand);
                os << stylize("]", help_element::Braces);
            }

            os << "\n";

            if (hasParentCommand)
            {
                unsigned cursor = 1;
                os << indent(2, &cursor);
                os << stylize(wordWrapped(com.helpText, cursor, margin, &cursor), help_element::HelpText)
                   << "\n\n";
            }
        }
        // }}}
        if (!com.options.empty() || com.verbatim.has_value()) // {{{ print options
        {
            os << indent(2) << stylize("Options:", help_element::Header) << "\n\n";

            auto const leftPadding = indent(3);
            auto const minRightPadSize = 2;
            auto const maxOptionTextSize = longestOptionText(com.options, style.optionStyle);
            auto const columnWidth =
                static_cast<unsigned>(leftPadding.size() + maxOptionTextSize + minRightPadSize);

            for (option const& option: com.options)
            {
                // if (option.deprecated)
                //     continue;

                auto const leftSize =
                    leftPadding.size() + printOption(option, nullopt, style.optionStyle).size();
                assert(columnWidth >= leftSize);
                auto const actualRightPaddingSize = columnWidth - leftSize;
                auto const left = leftPadding + printOption(option, style.colors, style.optionStyle)
                                  + spaces(actualRightPaddingSize);

                os << left;

                auto cursor = columnWidth + 1;
                os << stylize(wordWrapped(option.helpText, columnWidth, margin, &cursor),
                              help_element::HelpText);

                // {{{ append default value, if any
                // NB: It seems like fmt::format is having problems with
                //     formatting std::variant<>'s on some systems.'
                // auto const defaultValueStr = fmt::format("{}", option.value);
                auto const defaultValueStr = [&]() -> string {
                    if (holds_alternative<bool>(option.v))
                        return get<bool>(option.v) ? "true" : "false";
                    else if (holds_alternative<int>(option.v))
                        return std::to_string(get<int>(option.v));
                    else if (holds_alternative<unsigned int>(option.v))
                        return std::to_string(get<unsigned int>(option.v));
                    else if (holds_alternative<double>(option.v))
                        return std::to_string(get<double>(option.v));
                    else
                        return get<string>(option.v);
                }();
                if ((option.presence == presence::Optional && !defaultValueStr.empty())
                    || (holds_alternative<bool>(option.v) && get<bool>(option.v)))
                {
                    auto const defaultTextPrefix = string("default:");
                    auto const defaultText = stylize("[", help_element::Braces) + defaultTextPrefix + " "
                                             + stylize(defaultValueStr, help_element::OptionValue)
                                             + stylize("]", help_element::Braces);
                    auto const defaultTextLength =
                        1 + defaultTextPrefix.size() + 1 + defaultValueStr.size() + 1;
                    if (cursor + defaultTextLength > margin)
                        os << "\n" << spaces(columnWidth) << defaultText;
                    else
                        os << " " << defaultText;
                }
                // }}}

                os << '\n';
            }
            if (com.verbatim.has_value())
            {
                auto const& verbatim = com.verbatim.value();
                auto const leftSize =
                    static_cast<unsigned>(leftPadding.size() + 2 + verbatim.placeholder.size());
                assert(columnWidth > leftSize);
                auto const actualRightPaddingSize = columnWidth - leftSize;
                auto const left = leftPadding + stylize("[", help_element::Braces)
                                  + stylize(verbatim.placeholder, help_element::Verbatim)
                                  + stylize("]", help_element::Braces) + spaces(actualRightPaddingSize);

                os << left;
                auto cursor = columnWidth + 1;
                os << stylize(wordWrapped(verbatim.helpText, columnWidth, margin, &cursor),
                              help_element::HelpText);
                os << '\n';
            }
            os << '\n';
        }
        // }}}
        if (!com.children.empty()) // {{{ recurse to sub commands
        {
            parents.emplace_back(&com);
            for (command const& subcmd: com.children)
                detailedDescription(os, subcmd, style, margin, parents);
            parents.pop_back();
        } // }}}
    }

    void detailedDescription(ostream& os,
                             command const& com,
                             help_display_style const& style,
                             unsigned margin)
    {
        vector<command const*> parents;
        detailedDescription(os, com, style, margin, parents);
    }
} // namespace
// }}}

help_display_style::color_map help_display_style::defaultColors()
{
    return color_map {
        { help_element::Header, "\033[32;1;4:2m" },      { help_element::Braces, "\033[37;1m" },
        { help_element::OptionDash, "\033[34;1m" },      { help_element::OptionName, "\033[37m" },
        { help_element::OptionEqual, "\033[34;1m" },     { help_element::OptionValue, "\033[36m" },
        { help_element::ImplicitCommand, "\033[33;1m" }, { help_element::Verbatim, "\033[36m" },
        { help_element::HelpText, "\033[38m" },
    };
}

/**
 * Constructs a usage text suitable for printing out the command usage syntax in terminals.
 *
 * @param command The command to construct the usage text for.
 * @param colored Boolean indicating whether or not to colorize the output via VT sequences.
 * @param margin  Number of characters to write at most per line.
 */
string usageText(command const& com,
                 help_display_style const& style,
                 unsigned margin,
                 string const& cmdPrefix)
{
    auto const colorize = colorizer(style.colors);
    auto const indentationWidth = static_cast<unsigned>(cmdPrefix.size());

    auto const printOptionList = [&](ostream& os, option_list const& options, unsigned* cursor) {
        auto const indent = *cursor;
        for (option const& opt: options)
        {
            // if (option.deprecated)
            //     continue;

            os << ' ' << printOption(opt, style.colors, style.optionStyle, indent, margin, cursor);
        }
    };

    auto cursor = indentationWidth + 1;
    if (com.children.empty())
    {
        stringstream sstr;
        sstr << cmdPrefix;

        if (com.select == command_select::Explicit)
        {
            cursor += static_cast<unsigned>(com.name.size());
            sstr << com.name;
        }
        else
        {
            cursor += static_cast<unsigned>(com.name.size()) + 2;
            sstr << colorize("[", help_element::Braces);
            sstr << colorize(com.name, help_element::ImplicitCommand);
            sstr << colorize("]", help_element::Braces);
        }

        auto const indent = cursor;
        printOptionList(sstr, com.options, &cursor);

        if (com.verbatim.has_value())
        {
            if (cursor + 3 + com.verbatim.value().placeholder.size() > size_t(margin))
            {
                sstr << "\n";
                sstr << spaces(indent);
            }
            else
                sstr << ' ';

            sstr << colorize("[", help_element::Braces);
            sstr << colorize(com.verbatim.value().placeholder, help_element::Verbatim);
            sstr << colorize("]", help_element::Braces);
        }

        sstr << '\n';
        return sstr.str();
    }
    else
    {
        stringstream prefix;
        prefix << cmdPrefix << com.name;
        printOptionList(prefix, com.options, &cursor);
        prefix << ' ';

        string const prefixStr = prefix.str();
        stringstream sstr;
        for (command const& subcmd: com.children)
            sstr << usageText(subcmd, style, margin, prefixStr);
        if (com.children.empty())
            sstr << '\n';
        return sstr.str();
    }
}

string helpText(command const& command, help_display_style const& style, unsigned margin)
{
    auto const stylize = stylizer(style);

    stringstream output;

    output << stylize(command.helpText, help_element::HelpText) << "\n\n";

    output << "  " << stylize("Usage:", help_element::Header) << "\n\n";
    output << usageText(command, style, margin, indent(1));
    output << '\n';

    auto constexpr DescriptionHeader = string_view { "Detailed description:" };

    output << "  " << stylize(DescriptionHeader, help_element::Header) << "\n\n";
    detailedDescription(output, command, style, margin);

    return output.str();
}

} // namespace crispy::cli
