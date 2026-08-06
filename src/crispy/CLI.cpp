// SPDX-License-Identifier: Apache-2.0
#include <crispy/CLI.hpp>

#include <crispy/Assert.hpp>
#include <crispy/LogStore.hpp>
#include <crispy/Times.hpp>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <deque>
#include <ranges>
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
    struct ParseContext
    {
        StringViewList const& args;
        size_t pos = 0;

        deque<Command const*> currentCommand = {};
        Option const* currentOption = nullptr;

        FlagStore output = {};
    };

    auto namePrefix(ParseContext const& context, char delim = '.') -> string // {{{
    {
        string output;
        for (size_t i = 0; i < context.currentCommand.size(); ++i)
        {
            Command const* v = context.currentCommand.at(i);
            if (i != 0)
                output += delim;
            output += v->name;
        }

        return output;
    } //  }}}

    bool hasTokensAvailable(ParseContext const& context)
    {
        return context.pos < context.args.size();
    }

    auto currentToken(ParseContext const& context) -> string_view
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

    Option const* findOption(ParseContext const& context, string_view name)
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

    string_view consumeToken(ParseContext& context)
    {
        // NAME := <just a name>
        if (context.pos >= context.args.size())
            throw ParserError("Not enough arguments specified.");

        CLI_DEBUG(std::format("Consuming token '{}'", currentToken(context)));
        return context.args.at(context.pos++);
    }

    /// Parses the given parameter value @p text with respect to the given @p option.
    Value parseValue(ParseContext& context, string_view text) // {{{
    {
        // Value := STR | BOOL | FLOAT | INT | UINT

        // BOOL
        if (holds_alternative<bool>(context.currentOption->v))
        {
            if (isTrue(text))
                return Value { true };

            if (isFalse(text))
                return Value { false };

            throw ParserError("Boolean value expected but something else specified.");
        }

        // FLOAT
        try
        {
            if (holds_alternative<double>(context.currentOption->v))
            {
                return Value { stod(string(text)) }; // TODO: avoid malloc
            }
        }
        catch (...)
        {
            throw ParserError("Floating point value expected but something else specified.");
        }

        // UINT
        try
        {
            if (holds_alternative<unsigned>(context.currentOption->v))
                return Value { unsigned(stoul(string(text))) };
        }
        catch (...)
        {
            throw ParserError("Unsigned integer value expected but something else specified.");
        }

        // INT
        try
        {
            if (holds_alternative<int>(context.currentOption->v))
                return Value { stoi(string(text)) };
        }
        catch (...)
        {
            throw ParserError("Integer value expected but something else specified.");
        }

        // STR
        return Value { string(text) };
    } // }}}
    Value parseValue(ParseContext& context) // {{{
    {
        if (holds_alternative<bool>(context.currentOption->v))
        {
            auto const text = currentToken(context);
            if (isTrue(text))
            {
                consumeToken(context);
                return Value { true };
            }

            if (isFalse(text))
            {
                consumeToken(context);
                return Value { false };
            }

            // Booleans can be specified just by `--flag` or `flag` without any value
            // and are considered to be true (implicit).
            return Value { true };
        }
        return parseValue(context, consumeToken(context));
    } // }}}

    struct ScopedOption
    {
        ParseContext& context;
        ScopedOption(ParseContext& context, Option const& option): context { context }
        {
            context.currentOption = &option;
        }
        ~ScopedOption() { context.currentOption = nullptr; }
    };

    struct ScopedCommand
    {
        ParseContext& context;
        ScopedCommand(ParseContext& context, Command const& command): context { context }
        {
            context.currentCommand.emplace_back(&command);
        }
        ~ScopedCommand() { context.currentCommand.pop_back(); }
    };

    /// Tries parsing an option name and, if matching, also its value if provided.
    ///
    /// @throw ParserError on parserfailures
    /// @returns nullptr if current token is no option name a pair of an @c Option pointer and its optional
    /// value otherwise.
    optional<pair<Option const*, Value>> tryParseOption(ParseContext& context)
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
                if (Option const* opt = findOption(context, name)) // --NAME=VALUE
                {
                    consumeToken(context);
                    if (valueText.empty() && !holds_alternative<string>(opt->v))
                        throw ParserError("Explicit empty value passed but a non-string value expected.");

                    auto const optionScope = ScopedOption { context, *opt };
                    return pair { opt, parseValue(context, valueText) };
                }
            }
            else
            {
                auto const name = current.substr(2);
                if (Option const* opt = findOption(context, name)) // --NAME
                {
                    consumeToken(context);
                    auto const optionScope = ScopedOption { context, *opt };
                    return pair { opt, parseValue(context) };
                }
            }
        }
        else if (matchPrefix(current, "-")) // POSIX-style short opt (or otherwise ...)
        {
            auto const name = current.substr(1);
            if (Option const* opt = findOption(context, name)) // -NAME
            {
                consumeToken(context);
                auto const optionScope = ScopedOption { context, *opt };
                return pair { opt, parseValue(context) };
            }
        }
        else // Natural style option
        {
            auto const name = current;
            if (Option const* opt = findOption(context, name)) // -NAME
            {
                consumeToken(context);
                auto const optionScope = ScopedOption { context, *opt };
                return pair { opt, parseValue(context) };
            }
        }

        return nullopt;
    }

    void setOption(ParseContext& context, string const& key, Value value)
    {
        CLI_DEBUG(std::format("setOption({}): {}", key, value));
        context.output.values[key] = std::move(value);
    }

    void parseOptionList(ParseContext& context)
    {
        // Option := Option*
        auto const optionPrefix = namePrefix(context);

        while (true)
        {
            auto optionOptPair = tryParseOption(context);
            if (!optionOptPair.has_value())
                break;

            auto& [option, value] = optionOptPair.value();
            auto const fqdn = optionPrefix + "." + Name(option->name.longName);
            setOption(context, fqdn, std::move(value));
        }
    }

    auto tryLookupCommand(ParseContext const& context) -> Command const*
    {
        auto const token = matchPrefix(currentToken(context), "--") ? currentToken(context).substr(2)
                                                                    : currentToken(context);

        for (Command const& command: context.currentCommand.back()->children)
        {
            if (token == command.name)
                return &command;
        }

        return nullptr; // not found
    }

    auto tryImplicitCommand(ParseContext const& context) -> Command const*
    {
        for (Command const& command: context.currentCommand.back()->children)
            if (command.select == CommandSelect::Implicit)
            {
                CLI_DEBUG(std::format("Select implicit command {}.", command.name));
                return &command;
            }

        return nullptr;
    }

    void prefillDefaults(ParseContext& context, Command const& com)
    {
        auto const commandScope = ScopedCommand { context, com };
        auto const prefix = namePrefix(context) + ".";

        for (Option const& option: context.currentCommand.back()->options)
        {
            if (option.presence == Presence::Required)
                continue; // Do not prefill options that are required anyways.

            auto const fqdn = prefix + Name(option.name.longName);
            context.output.values[fqdn] = option.v;
            setOption(context, fqdn, option.v);
        }

        for (Command const& subcmd: com.children)
        {
            auto const fqdn = prefix + Name(subcmd.name);
            setOption(context, fqdn, Value { false });

            prefillDefaults(context, subcmd);
        }
    }

    auto parseCommand(Command const& com, ParseContext& context) -> bool
    {
        // command := NAME Option* Section*
        auto const commandScope = ScopedCommand { context, com };
        context.output.values[namePrefix(context)] = Value { true };

        parseOptionList(context);

        if (Command const* subcmd = tryLookupCommand(context))
        {
            CLI_DEBUG(std::format("parseCommand: found sub command: {}", subcmd->name));
            consumeToken(context); // Name was already ensured to be right (or is assumed to be right).
            parseCommand(*subcmd, context);
        }
        else if (Command const* subcmd = tryImplicitCommand(context))
        {
            CLI_DEBUG(std::format("parseCommand: found implicit sub command: {}", subcmd->name));
            // DO not consume token
            parseCommand(*subcmd, context);
        }
        else if (com.verbatim.has_value())
        {
            CLI_DEBUG(std::format("parseCommand: going verbatim."));
            if (hasTokensAvailable(context))
            {
                if (currentToken(context) == "--")
                    (void) consumeToken(context); // consume "--"
                while (context.pos < context.args.size())
                    context.output.verbatim.emplace_back(consumeToken(context));
            }
        }

        if (context.pos == context.args.size())
            context.output.values[namePrefix(context)] = Value { true };

        // A command must not leave any trailing tokens at the end of parsing
        return context.pos == context.args.size();
    }

    StringViewList stringViewList(int argc, char const* const* argv)
    {
        StringViewList output;
        output.resize(static_cast<unsigned>(argc));

        for (auto const i: std::views::iota(0u, static_cast<unsigned>(argc)))
            output[i] = argv[i];

        return output;
    }

    void validate(Command const& com, ParseContext& context, string const& keyPrefix)
    {
        auto const key = keyPrefix.empty() ? string(com.name) : std::format("{}.{}", keyPrefix, com.name);

        // Ensure all required fields are provided for those commands that have been provided.
        for (Option const& option: com.options)
        {
            auto const optionKey = std::format("{}.{}", key, option.name.longName);
            // NOLINTNEXTLINE(readability-container-contains)
            if (option.presence == Presence::Required && !context.output.values.count(optionKey))
                throw invalid_argument(std::format("Missing option: {}", optionKey));
        }

        for (Command const& subcmd: com.children)
        {
            auto const commandKey = std::format("{}.{}", key, subcmd.name);
            if (context.output.get<bool>(commandKey))
                validate(subcmd, context, key);
        }
    }

} // namespace
// }}}

void validate(Command const& command)
{
    (void) command;
    // TODO: throw if command is not well defined.
    //
    // - no duplicated nems in same scope
    // - names must not start with '-' (dash)
    // - must not contain '='
}

optional<FlagStore> parse(Command const& command, StringViewList const& args)
{
    validate(command);

    auto context = ParseContext { .args = args };

    prefillDefaults(context, command);

    // XXX do not enforce checking the first token, as for main()'s argv[0] this most likely is different.
    // if (currentToken(context) != command.name)
    //     return nullopt;

    consumeToken(context); // Name was already ensured to be right (or is assumed to be right).
    if (!parseCommand(command, context))
        return nullopt;

    // auto const& flags = context.output;
    // std::cout << std::format("Flags: {}\n", flags.values.size());
    // for (auto const & [k, v] : flags.values)
    //     std::cout << std::format(" - {}: {}\n", k, v);

    validate(command, context, "");

    return std::move(context.output);
}

optional<FlagStore> parse(Command const& command, int argc, char const* const* argv)
{
    return parse(command, stringViewList(argc, argv));
}

} // namespace crispy::cli

namespace crispy::cli
{

namespace // {{{ helpers
{
    auto spaces(size_t count)
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
    auto stylizer(HelpDisplayStyle const& style) -> function<string(string_view, HelpElement)>
    {
        return [style](string_view text, HelpElement element) -> string {
            auto const [pre, post] = [&]() -> pair<string_view, string_view> {
                // NOLINTNEXTLINE(readability-container-contains)
                if (style.colors.has_value() && style.colors.value().count(element))
                    return { style.colors.value().at(element), "\033[m"sv };
                else
                    return { ""sv, ""sv };
            }();

            if (!style.hyperlink)
                return std::format("{}{}{}", pre, text, post);

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

    auto colorizer(optional<HelpDisplayStyle::ColorMap> const& colors)
        -> function<string(string_view, HelpElement)>
    {
        HelpDisplayStyle style {};
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

    string printParam(optional<HelpDisplayStyle::ColorMap> const& colors,
                      OptionStyle optionStyle,
                      OptionName const& name,
                      string_view placeholder,
                      Presence presence)
    {
        auto const colorize = colorizer(colors);

        stringstream os;

        if (presence == Presence::Optional)
            os << colorize("[", HelpElement::Braces);
        switch (optionStyle)
        {
            case OptionStyle::Natural:
                // if (name.shortName)
                // {
                //     os << colorize(string(1, name.shortName), HelpElement::OptionName);
                //     os << ", ";
                // }
                os << colorize(name.longName, HelpElement::OptionName);
                if (!placeholder.empty())
                    os << ' ' << colorize(placeholder, HelpElement::OptionValue);
                break;
            case OptionStyle::Posix:
                if (name.shortName != '\0')
                {
                    os << colorize("-", HelpElement::OptionDash);
                    os << colorize(string(1, name.shortName), HelpElement::OptionName);
                    os << ", ";
                }
                os << colorize("--", HelpElement::OptionDash)
                   << colorize(name.longName, HelpElement::OptionName);
                if (!placeholder.empty())
                    os << colorize("=", HelpElement::OptionEqual)
                       << colorize(placeholder, HelpElement::OptionValue);
                break;
        }
        if (presence == Presence::Optional)
            os << colorize("]", HelpElement::Braces);

        return os.str();
    }

    string printOption(Option const& option,
                       optional<HelpDisplayStyle::ColorMap> const& colors,
                       OptionStyle optionStyle)
    {
        // TODO: make use of option.placeholder
        auto const placeholder = [](Option const& option, string_view type) -> string_view {
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

    string printOption(Option const& option,
                       optional<HelpDisplayStyle::ColorMap> const& colors,
                       OptionStyle displayStyle,
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

    size_t longestOptionText(OptionList const& options, OptionStyle displayStyle)
    {
        size_t result = 0;
        for (Option const& option: options)
            result = max(result, printOption(option, nullopt, displayStyle).size());
        return result;
    }

    void printCommandSequence(ostream& os,
                              Command const& com,
                              unsigned margin,
                              vector<Command const*>& parents,
                              auto const& stylize)
    {
        os << indent(1);
        for (Command const* parent: parents)
            os << stylize(parent->name, HelpElement::OptionValue /*well, yeah*/) << ' ';

        if (com.select == CommandSelect::Explicit)
            os << com.name;
        else
        {
            os << stylize("[", HelpElement::Braces);
            os << stylize(com.name, HelpElement::ImplicitCommand);
            os << stylize("]", HelpElement::Braces);
        }

        os << "\n";

        if (!parents.empty())
        {
            unsigned cursor = 1;
            os << indent(2, &cursor);
            os << stylize(wordWrapped(com.helpText, cursor, margin, &cursor), HelpElement::HelpText)
               << "\n\n";
        }
    }

    void printOptions(ostream& os,
                      Command const& com,
                      HelpDisplayStyle const& style,
                      unsigned margin,
                      [[maybe_unused]] vector<Command const*>& parents)
    {
        auto const stylize = stylizer(style);

        os << indent(2) << stylize("Options:", HelpElement::Header) << "\n\n";

        auto const leftPadding = indent(3);
        auto const minRightPadSize = 2;
        auto const maxOptionTextSize = longestOptionText(com.options, style.optionStyle);
        auto const columnWidth =
            static_cast<unsigned>(leftPadding.size() + maxOptionTextSize + minRightPadSize);

        for (Option const& option: com.options)
        {
            // if (option.deprecated)
            //     continue;

            auto const leftSize = leftPadding.size() + printOption(option, nullopt, style.optionStyle).size();
            assert(columnWidth >= leftSize);
            auto const actualRightPaddingSize = columnWidth - leftSize;
            auto const left = leftPadding + printOption(option, style.colors, style.optionStyle)
                              + spaces(actualRightPaddingSize);

            os << left;

            auto cursor = columnWidth + 1;
            os << stylize(wordWrapped(option.helpText, columnWidth, margin, &cursor), HelpElement::HelpText);

            // {{{ append default value, if any
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
            if ((option.presence == Presence::Optional && !defaultValueStr.empty())
                || (holds_alternative<bool>(option.v) && get<bool>(option.v)))
            {
                auto const defaultTextPrefix = string("default:");
                auto const defaultText = stylize("[", HelpElement::Braces) + defaultTextPrefix + " "
                                         + stylize(defaultValueStr, HelpElement::OptionValue)
                                         + stylize("]", HelpElement::Braces);
                auto const defaultTextLength = 1 + defaultTextPrefix.size() + 1 + defaultValueStr.size() + 1;
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
            auto const leftSize = static_cast<unsigned>(leftPadding.size() + 2 + verbatim.placeholder.size());
            assert(columnWidth > leftSize);
            auto const actualRightPaddingSize = columnWidth - leftSize;
            auto const left = leftPadding + stylize("[", HelpElement::Braces)
                              + stylize(verbatim.placeholder, HelpElement::Verbatim)
                              + stylize("]", HelpElement::Braces) + spaces(actualRightPaddingSize);

            os << left;
            auto cursor = columnWidth + 1;
            os << stylize(wordWrapped(verbatim.helpText, columnWidth, margin, &cursor),
                          HelpElement::HelpText);
            os << '\n';
        }
        os << '\n';
    }

    void detailedDescription(ostream& os,
                             Command const& com,
                             HelpDisplayStyle const& style,
                             unsigned margin,
                             [[maybe_unused]] vector<Command const*>& parents)
    {
        // NOTE: We assume that cursor position is at first column!
        auto const stylize = stylizer(style);
        bool const isLeafCommand = com.children.empty();

        if (isLeafCommand || !com.options.empty() || com.verbatim.has_value()) // {{{ print command sequence
        {
            printCommandSequence(os, com, margin, parents, stylize);
        }
        // }}}
        if (!com.options.empty() || com.verbatim.has_value()) // {{{ print options
        {
            printOptions(os, com, style, margin, parents);
        }
        // }}}
        if (!com.children.empty()) // {{{ recurse to sub commands
        {
            parents.emplace_back(&com);
            for (Command const& subcmd: com.children)
                detailedDescription(os, subcmd, style, margin, parents);
            parents.pop_back();
        } // }}}
    }

    void detailedDescription(ostream& os, Command const& com, HelpDisplayStyle const& style, unsigned margin)
    {
        vector<Command const*> parents;
        detailedDescription(os, com, style, margin, parents);
    }
} // namespace
// }}}

HelpDisplayStyle::ColorMap HelpDisplayStyle::defaultColors()
{
    return ColorMap {
        { HelpElement::Header, "\033[32;1;4:2m" },      { HelpElement::Braces, "\033[37;1m" },
        { HelpElement::OptionDash, "\033[34;1m" },      { HelpElement::OptionName, "\033[37m" },
        { HelpElement::OptionEqual, "\033[34;1m" },     { HelpElement::OptionValue, "\033[36m" },
        { HelpElement::ImplicitCommand, "\033[33;1m" }, { HelpElement::Verbatim, "\033[36m" },
        { HelpElement::HelpText, "\033[38m" },
    };
}

/**
 * Constructs a usage text suitable for printing out the command usage syntax in terminals.
 *
 * @param command The command to construct the usage text for.
 * @param colored Boolean indicating whether or not to colorize the output via VT sequences.
 * @param margin  Number of characters to write at most per line.
 */
string usageText(Command const& com, HelpDisplayStyle const& style, unsigned margin, string const& cmdPrefix)
{
    auto const colorize = colorizer(style.colors);
    auto const indentationWidth = static_cast<unsigned>(cmdPrefix.size());

    auto const printOptionList = [&](ostream& os, OptionList const& options, unsigned* cursor) {
        auto const indent = *cursor;
        for (Option const& opt: options)
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

        if (com.select == CommandSelect::Explicit)
        {
            cursor += static_cast<unsigned>(com.name.size());
            sstr << com.name;
        }
        else
        {
            cursor += static_cast<unsigned>(com.name.size()) + 2;
            sstr << colorize("[", HelpElement::Braces);
            sstr << colorize(com.name, HelpElement::ImplicitCommand);
            sstr << colorize("]", HelpElement::Braces);
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

            sstr << colorize("[", HelpElement::Braces);
            sstr << colorize(com.verbatim.value().placeholder, HelpElement::Verbatim);
            sstr << colorize("]", HelpElement::Braces);
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
        for (Command const& subcmd: com.children)
            sstr << usageText(subcmd, style, margin, prefixStr);
        if (com.children.empty())
            sstr << '\n';
        return sstr.str();
    }
}

string helpText(Command const& command, HelpDisplayStyle const& style, unsigned margin)
{
    auto const stylize = stylizer(style);

    stringstream output;

    output << stylize(command.helpText, HelpElement::HelpText) << "\n\n";

    output << "  " << stylize("Usage:", HelpElement::Header) << "\n\n";
    output << usageText(command, style, margin, indent(1));
    output << '\n';

    auto constexpr DescriptionHeader = string_view { "Detailed description:" };

    output << "  " << stylize(DescriptionHeader, HelpElement::Header) << "\n\n";
    detailedDescription(output, command, style, margin);

    return output.str();
}

} // namespace crispy::cli
