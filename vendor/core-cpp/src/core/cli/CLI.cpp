// SPDX-License-Identifier: Apache-2.0
#include <core/cli/CLI.hpp>

#include <core/Assert.hpp>
#include <core/Times.hpp>
#include <core/log/LogStore.hpp>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <deque>
#include <expected>
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
using std::map;
using std::max;
using std::nullopt;
using std::optional;
using std::ostream;
using std::pair;
using std::string;
using std::string_view;
using std::stringstream;
using std::vector;

using namespace std::string_view_literals;
using namespace std::string_literals;

namespace core::cli
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
        for (auto const i: std::views::iota(size_t { 0 }, context.currentCommand.size()))
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
        return text.starts_with(prefix);
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

    /// The refusal of the token at @p index.
    auto refuse(ParseErrorKind kind, size_t index, string message) -> std::unexpected<ParseError>
    {
        return std::unexpected { ParseError {
            .kind = kind, .tokenIndex = index, .message = std::move(message) } };
    }

    auto consumeToken(ParseContext& context) -> std::expected<string_view, ParseError>
    {
        // NAME := <just a name>
        if (context.pos >= context.args.size())
            return refuse(ParseErrorKind::NotEnoughArguments, context.pos, "Not enough arguments specified.");

        CLI_DEBUG(std::format("Consuming token '{}'", currentToken(context)));
        return context.args.at(context.pos++);
    }

    /// Reads all of @p text as a number of type @p T, or nothing.
    template <typename T>
    auto parseNumber(string_view text) -> optional<T>
    {
        auto value = T {};
        auto const* const first = text.data();
        auto const* const last = first + text.size();
        auto const [end, error] = std::from_chars(first, last, value);
        if (error != std::errc {} || end != last)
            return nullopt;
        return value;
    }

    /// Reads all of @p text as a double, or nothing. `std::strtod` rather than `std::from_chars`,
    /// whose floating-point overloads libc++ 17 -- the WebAssembly build's -- does not provide.
    auto parseDouble(string_view text) -> optional<double>
    {
        if (text.empty() || std::isspace(static_cast<unsigned char>(text.front())) != 0)
            return nullopt;
        auto const owned = string(text);
        char* end = nullptr;
        errno = 0;
        auto const value = std::strtod(owned.c_str(), &end);
        if (errno == ERANGE || end != owned.c_str() + owned.size())
            return nullopt;
        return value;
    }

    /// Parses the given parameter value @p text, the token at @p index, with respect to the current
    /// option.
    auto parseValue(ParseContext const& context, string_view text, size_t index)
        -> std::expected<Value, ParseError> // {{{
    {
        // Value := STR | BOOL | FLOAT | INT | UINT
        auto const& option = *context.currentOption;
        auto const invalid = [&](string_view expected) {
            return refuse(
                ParseErrorKind::InvalidValue,
                index,
                std::format(R"(Option "{}" expects {}, not "{}".)", option.name.longName, expected, text));
        };

        // BOOL
        if (holds_alternative<bool>(option.v))
        {
            if (isTrue(text))
                return Value { true };

            if (isFalse(text))
                return Value { false };

            return invalid("a boolean");
        }

        // FLOAT
        if (holds_alternative<double>(option.v))
        {
            if (auto const value = parseDouble(text))
                return Value { *value };
            return invalid("a floating-point number");
        }

        // UINT
        if (holds_alternative<unsigned>(option.v))
        {
            if (auto const value = parseNumber<unsigned>(text))
                return Value { *value };
            return invalid("an unsigned integer");
        }

        // INT
        if (holds_alternative<int>(option.v))
        {
            if (auto const value = parseNumber<int>(text))
                return Value { *value };
            return invalid("an integer");
        }

        // STR
        return Value { string(text) };
    } // }}}
    auto parseValue(ParseContext& context) -> std::expected<Value, ParseError> // {{{
    {
        if (holds_alternative<bool>(context.currentOption->v))
        {
            auto const text = currentToken(context);
            if (isTrue(text))
            {
                ++context.pos;
                return Value { true };
            }

            if (isFalse(text))
            {
                ++context.pos;
                return Value { false };
            }

            // Booleans can be specified just by `--flag` or `flag` without any value
            // and are considered to be true (implicit).
            return Value { true };
        }
        if (!hasTokensAvailable(context))
            return refuse(
                ParseErrorKind::NotEnoughArguments,
                context.pos,
                std::format("Option \"{}\" expects a value.", context.currentOption->name.longName));
        auto const index = context.pos++;
        return parseValue(context, context.args.at(index), index);
    } // }}}

    struct ScopedOption
    {
        ParseContext& context;
        ScopedOption(ParseContext& parseContext, Option const& option): context { parseContext }
        {
            context.currentOption = &option;
        }
        ~ScopedOption() { context.currentOption = nullptr; }
    };

    struct ScopedCommand
    {
        ParseContext& context;
        ScopedCommand(ParseContext& parseContext, Command const& command): context { parseContext }
        {
            context.currentCommand.emplace_back(&command);
        }
        ~ScopedCommand() { context.currentCommand.pop_back(); }
    };

    /// An option and the value given for it.
    using ParsedOption = pair<Option const*, Value>;

    /// Parses the value of @p option, whose name is the current token, and consumes both.
    auto parseOptionValue(ParseContext& context, Option const& option)
        -> std::expected<optional<ParsedOption>, ParseError>
    {
        ++context.pos; // the name, which findOption() has matched
        auto const optionScope = ScopedOption { context, option };
        return parseValue(context).transform(
            [&](Value value) { return optional { ParsedOption { &option, std::move(value) } }; });
    }

    /// Tries parsing an option name and, if matching, also its value if provided.
    ///
    /// @returns nullopt if the current token is no option name, a pair of an @c Option pointer and
    /// its value otherwise, or the error that made the option's value unreadable.
    auto tryParseOption(ParseContext& context) -> std::expected<optional<ParsedOption>, ParseError>
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
                    auto const index = context.pos++;
                    if (valueText.empty() && !holds_alternative<string>(opt->v))
                        return refuse(ParseErrorKind::EmptyValue,
                                      index,
                                      std::format("Option \"{}\" was given an explicit empty value, but its "
                                                  "value is not a string.",
                                                  opt->name.longName));

                    auto const optionScope = ScopedOption { context, *opt };
                    return parseValue(context, valueText, index).transform([&](Value value) {
                        return optional { ParsedOption { opt, std::move(value) } };
                    });
                }
            }
            else if (Option const* opt = findOption(context, current.substr(2))) // --NAME
                return parseOptionValue(context, *opt);
        }
        else if (matchPrefix(current, "-")) // POSIX-style short opt (or otherwise ...)
        {
            if (Option const* opt = findOption(context, current.substr(1))) // -NAME
                return parseOptionValue(context, *opt);
        }
        else if (Option const* opt = findOption(context, current)) // Natural style option: NAME
            return parseOptionValue(context, *opt);

        return optional<ParsedOption> {};
    }

    void setOption(ParseContext& context, string const& key, Value value)
    {
        CLI_DEBUG(std::format("setOption({}): {}", key, value));
        context.output.values[key] = std::move(value);
    }

    auto parseOptionList(ParseContext& context) -> std::expected<void, ParseError>
    {
        // Option := Option*
        auto const optionPrefix = namePrefix(context);

        while (true)
        {
            auto parsed = tryParseOption(context);
            if (!parsed)
                return std::unexpected { std::move(parsed).error() };
            if (!parsed->has_value())
                return {};

            auto& [option, value] = parsed->value();
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
            setOption(context, fqdn, option.v);
        }

        for (Command const& subcmd: com.children)
        {
            auto const fqdn = prefix + Name(subcmd.name);
            setOption(context, fqdn, Value { false });

            prefillDefaults(context, subcmd);
        }
    }

    /// Parses @p com, whose name has been consumed, and whatever sub-command follows it.
    auto parseCommand(Command const& com, ParseContext& context) -> std::expected<void, ParseError>
    {
        // command := NAME Option* Section*
        auto const commandScope = ScopedCommand { context, com };
        context.output.values[namePrefix(context)] = Value { true };

        if (auto options = parseOptionList(context); !options)
            return options;

        if (Command const* subcmd = tryLookupCommand(context))
        {
            CLI_DEBUG(std::format("parseCommand: found sub command: {}", subcmd->name));
            ++context.pos; // Name was already ensured to be right (or is assumed to be right).
            return parseCommand(*subcmd, context);
        }

        if (Command const* implicitCommand = tryImplicitCommand(context))
        {
            CLI_DEBUG(std::format("parseCommand: found implicit sub command: {}", implicitCommand->name));
            // DO not consume token
            return parseCommand(*implicitCommand, context);
        }

        if (com.verbatim.has_value())
        {
            CLI_DEBUG(std::format("parseCommand: going verbatim."));
            if (hasTokensAvailable(context) && currentToken(context) == "--")
                ++context.pos; // consume "--"
            while (hasTokensAvailable(context))
                context.output.verbatim.emplace_back(context.args.at(context.pos++));
        }

        return {};
    }

    StringViewList stringViewList(int argc, char const* const* argv)
    {
        StringViewList output;
        output.resize(static_cast<unsigned>(argc));

        for (auto const i: std::views::iota(0u, static_cast<unsigned>(argc)))
            output[i] = argv[i];

        return output;
    }

    auto validate(Command const& com, ParseContext const& context, string const& keyPrefix)
        -> std::expected<void, ParseError>
    {
        auto const key = keyPrefix.empty() ? string(com.name) : std::format("{}.{}", keyPrefix, com.name);

        // Ensure all required fields are provided for those commands that have been provided.
        for (Option const& option: com.options)
        {
            auto const optionKey = std::format("{}.{}", key, option.name.longName);
            if (option.presence == Presence::Required && !context.output.values.contains(optionKey))
                return refuse(ParseErrorKind::MissingRequiredOption,
                              context.args.size(),
                              std::format("Missing option: {}", optionKey));
        }

        for (Command const& subcmd: com.children)
        {
            auto const commandKey = std::format("{}.{}", key, subcmd.name);
            if (!context.output.get<bool>(commandKey))
                continue;
            if (auto valid = validate(subcmd, context, key); !valid)
                return valid;
        }
        return {};
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

std::expected<FlagStore, ParseError> parse(Command const& command, StringViewList const& args)
{
    validate(command);

    auto context = ParseContext { .args = args };

    prefillDefaults(context, command);

    // The first token is the command's name, and is not checked: for main()'s argv[0] it most likely
    // is something else.
    return consumeToken(context)
        .and_then([&](string_view) { return parseCommand(command, context); })
        .and_then([&]() -> std::expected<void, ParseError> {
            // A command must not leave any trailing tokens at the end of parsing.
            if (hasTokensAvailable(context))
                return refuse(ParseErrorKind::UnexpectedToken,
                              context.pos,
                              std::format("Unexpected argument \"{}\".", currentToken(context)));
            return {};
        })
        .and_then([&] { return validate(command, context, ""); })
        .transform([&] { return std::move(context.output); });
}

std::expected<FlagStore, ParseError> parse(Command const& command, int argc, char const* const* argv)
{
    return parse(command, stringViewList(argc, argv));
}

} // namespace core::cli

namespace core::cli
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
                if (style.colors.has_value() && style.colors.value().contains(element))
                    return { style.colors.value().at(element), "\033[m"sv };
                else
                    return { ""sv, ""sv };
            }();

            if (!style.hyperlink)
                return std::format("{}{}{}", pre, text, post);

            string output;
            size_t a = 0;
            while (true)
            {
                size_t const b = text.find("://", a);
                if (b == std::string_view::npos || (b == 0))
                    break;

                size_t left = b;
                // Through unsigned char: isalpha() is undefined for a negative value, and every
                // continuation byte of a UTF-8 sequence in a help text is one.
                while (left > 0 && std::isalpha(static_cast<unsigned char>(text.at(left - 1))) != 0)
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

    /// One line's worth of a text, and how much of that text it stands for.
    struct WrappedChunk
    {
        /// What to write out.
        string_view text;
        /// How much of the input the chunk accounts for: the emitted text plus anything trimmed
        /// off it. The caller advances by THIS, never by `text.size()`. Advancing by the emitted
        /// length left the trimmed bytes in front of the index, and the loop that skips what a
        /// chunk stopped at skips line feeds, not spaces -- so a space before a line feed
        /// reproduced the same empty chunk for ever, and `--help` never returned.
        size_t consumed;
        /// Whether the next turn skips leading spaces (true) or the line feed this chunk stopped
        /// at (false).
        bool trimLeadingWhitespaces;
    };

    WrappedChunk nextWrappedChunk(string_view text, unsigned margin, unsigned cursor)
    {
        auto const linefeed = text.find('\n');
        if (linefeed != string_view::npos)
        {
            // The chunk ends at the line feed, with its trailing spaces trimmed off. Counted
            // down from the line feed rather than up from `linefeed - 1`, which is SIZE_MAX
            // when the text begins with one. The line feed itself is left for the caller's skip
            // loop, which is what guarantees progress when this chunk is empty.
            auto end = linefeed;
            while (end > 0 && text[end - 1] == ' ')
                --end;
            return { .text = text.substr(0, end), .consumed = linefeed, .trimLeadingWhitespaces = false };
        }

        auto const unwrappedLength = cursor + text.size();
        if (unwrappedLength <= margin)
            return { .text = text, .consumed = text.size(), .trimLeadingWhitespaces = true };

        // How much of the line is left. This was `margin - cursor + 1` with a `<= 0` guard below
        // it, which is dead for an unsigned type: a cursor at or past the margin -- an option
        // column wider than the terminal, or a pty reporting no width at all -- wrapped to about
        // four billion, and the index below walked off the end of the text.
        auto const available = margin > cursor ? margin - cursor + 1 : 1u;
        if (available >= text.size())
            return { .text = text, .consumed = text.size(), .trimLeadingWhitespaces = true };

        // Cut at the right margin, then shift left until we've hit a whitespace character.
        auto i = static_cast<size_t>(available - 1);
        while (i > 0 && (text[i] != ' ' && text[i] != '\n'))
            --i;

        // A word longer than the line has no whitespace to shift to. Cut it hard: an empty chunk
        // makes no progress, and the caller loops until it has emitted an indent per iteration
        // for as long as the process lives.
        auto const cut = i > 0 ? i : static_cast<size_t>(available);
        return { .text = text.substr(0, cut), .consumed = cut, .trimLeadingWhitespaces = true };
    }

    string wordWrapped(string_view text, unsigned indent, unsigned margin, unsigned* cursor)
    {
        string output;
        size_t i = 0;
        bool trimLeadingWhitespaces = true;
        while (true)
        {
            auto const trimChar = trimLeadingWhitespaces ? ' ' : '\n';
            while (i < text.size() && text[i] == trimChar)
                ++i; // skip leading whitespaces

            auto const chunk = nextWrappedChunk(text.substr(i), margin, *cursor);
            trimLeadingWhitespaces = chunk.trimLeadingWhitespaces;

            output += chunk.text;
            // The cursor counts emitted columns; the index counts consumed input. They differ by
            // whatever the chunk trimmed, which is why they are two numbers and not one.
            *cursor += static_cast<unsigned>(chunk.text.size());
            i += chunk.consumed;

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
        auto const placeholder = [](Option const& opt, string_view type) -> string_view {
            return !opt.placeholder.empty() ? opt.placeholder : type;
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
        auto const minRightPadSize = size_t { 2 };
        // The verbatim row shares this column, so its placeholder (plus its two brackets) decides
        // the width as much as any option does. Measured against the options alone, a placeholder
        // longer than the longest option underflowed the padding below into a string of about
        // four billion spaces.
        auto const verbatimTextSize = com.verbatim.has_value() ? com.verbatim->placeholder.size() + 2 : 0;
        auto const maxOptionTextSize =
            max(longestOptionText(com.options, style.optionStyle), verbatimTextSize);
        auto const columnWidth =
            static_cast<unsigned>(leftPadding.size() + maxOptionTextSize + minRightPadSize);

        for (Option const& option: com.options)
        {
            // if (option.deprecated)
            //     continue;

            auto const leftSize = leftPadding.size() + printOption(option, nullopt, style.optionStyle).size();
            auto const actualRightPaddingSize =
                columnWidth > leftSize ? columnWidth - leftSize : size_t { 1 };
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
            auto const actualRightPaddingSize = columnWidth > leftSize ? columnWidth - leftSize : 1u;
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
        // (This is the `else` of `if (com.children.empty())`, so there is no empty case to
        // terminate with a line feed here.)
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

} // namespace core::cli
