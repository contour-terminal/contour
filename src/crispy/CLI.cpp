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
#include "CLI.h"

#include <crispy/times.h>

#include <algorithm>
#include <cassert>
#include <deque>
#include <sstream>

#if 0 // !defined(NDEBUG)
    #include <iostream>
    #define CLI_DEBUG(that) do { std::cerr << (that) << std::endl; } while (0)
#else
    #define CLI_DEBUG(that) do { } while (0)
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

namespace crispy::cli // {{{ Parser
{

namespace // {{{ helper
{
    struct ParseContext {
        StringViewList const& args;
        size_t pos = 0;

        deque<Command const*> currentCommand = {};
        Option const* currentOption = nullptr;

        FlagStore output = {};
    };

    auto namePrefix(ParseContext const& _context, char _delim = '.') -> string // {{{
    {
        string output;
        for (size_t i = 0; i < _context.currentCommand.size(); ++i) // TODO: use crispy::indexed()
        {
            Command const* v = _context.currentCommand.at(i);
            if (i)
                output += _delim;
            output += v->name;
        }

        return output;
    } //  }}}

    auto currentToken(ParseContext const& _context) -> string_view
    {
        if (_context.pos >= _context.args.size())
            return string_view{}; // not enough arguments available

        return _context.args.at(_context.pos);
    }

    auto isTrue(string_view _token) -> bool
    {
        return _token == "true" || _token == "yes";
    }

    auto isFalse(string_view _token) -> bool
    {
        return _token == "false" || _token == "no";
    }

    bool matchPrefix(string_view _text, string_view _prefix)
    {
        if (_text.size() < _prefix.size())
            return false;
        for (size_t i = 0; i < _prefix.size(); ++i)
            if (_text[i] != _prefix[i])
                return false;
        return true;
    }

    Option const* findOption(ParseContext const& _context, string_view _name)
    {
        for (auto const& option : _context.currentCommand.back()->options)
            if (option.name == _name)
                return &option;
        return nullptr;
    }

    string_view consumeToken(ParseContext& _context)
    {
        // NAME := <just a name>
        if (_context.pos >= _context.args.size())
            throw ParserError("Not enough arguments specified.");

        CLI_DEBUG(fmt::format("Consuming token '{}'", currentToken(_context)));
        return _context.args.at(_context.pos++);
    }

    /// Parses the given parameter value @p _text with respect to the given @p _option.
    Value parseValue(ParseContext& _context, string_view _text) // {{{
    {
        // Value := STR | BOOL | FLOAT | INT | UINT

        // BOOL
        if (holds_alternative<bool>(_context.currentOption->value))
        {
            if (isTrue(_text))
                return Value{true};

            if (isFalse(_text))
                return Value{false};

            throw ParserError("Boolean value expected but something else specified.");
        }

        // FLOAT
        try {
            auto result = Value{stod(string(_text))}; // TODO: avoid malloc
            if (holds_alternative<double>(_context.currentOption->value))
                return result;
            throw ParserError("Floating point value expected but something else specified.");
        }
        catch (...) {}

        // UINT
        try {
            auto const result = stoul(string(_text));
            if (holds_alternative<unsigned>(_context.currentOption->value))
                return Value{unsigned(result)};
            if (holds_alternative<int>(_context.currentOption->value))
                return Value{int(result)};
            throw ParserError("Unsigned integer value expected but something else specified.");
        }
        catch (...) {}

        // INT
        try {
            auto const result = stoi(string(_text));
            if (holds_alternative<int>(_context.currentOption->value))
                return Value{int(result)};
            throw ParserError("Integer value expected but something else specified.");
        }
        catch (...) {}

        // STR
        return Value{string(_text)};
    } // }}}
    Value parseValue(ParseContext& _context) // {{{
    {
        if (holds_alternative<bool>(_context.currentOption->value))
        {
            auto const text = currentToken(_context);
            if (isTrue(text))
            {
                consumeToken(_context);
                return Value{true};
            }

            if (isFalse(text))
            {
                consumeToken(_context);
                return Value{false};
            }

            // Booleans can be specified just by `--flag` or `flag` without any value
            // and are considered to be true (implicit).
            return Value{true};
        }
        return parseValue(_context, consumeToken(_context));
    } // }}}

    struct ScopedOption {
        ParseContext& context;
        ScopedOption(ParseContext& _context, Option const& _option) : context{_context} { context.currentOption = &_option; }
        ~ScopedOption() { context.currentOption = nullptr; }
    };

    struct ScopedCommand {
        ParseContext& context;
        ScopedCommand(ParseContext& _context, Command const& _command) : context{_context} { context.currentCommand.emplace_back(&_command); }
        ~ScopedCommand() { context.currentCommand.pop_back(); }
    };

    /// Tries parsing an option name and, if matching, also its value if provided.
    ///
    /// @throw ParserError on parserfailures
    /// @returns nullptr if current token is no option name a pair of an @c Option pointer and its optional value otherwise.
    optional<pair<Option const*, Value>> tryParseOption(ParseContext& _context)
    {
        // NAME [VALUE]
        // -NAME [VALUE]
        // --NAME[=VALUE]
        auto const current = currentToken(_context);
        if (matchPrefix(current, "--")) // POSIX-style long-option
        {
            if (auto const i = current.find('='); i != current.npos)
            {
                auto const name = current.substr(2, i - 2);
                auto const valueText = current.substr(i + 1);
                if (Option const* opt = findOption(_context, name)) // --NAME=VALUE
                {
                    consumeToken(_context);
                    if (valueText.empty() && !holds_alternative<string>(opt->value))
                        throw ParserError("Explicit empty value passed but a non-string value expected.");

                    auto const _optionScope = ScopedOption{_context, *opt};
                    return pair{opt, parseValue(_context, valueText)};
                }
            }
            else
            {
                auto const name = current.substr(2);
                if (Option const* opt = findOption(_context, name)) // --NAME
                {
                    consumeToken(_context);
                    auto const _optionScope = ScopedOption{_context, *opt};
                    return pair{opt, parseValue(_context)};
                }
            }
        }
        else if (matchPrefix(current, "-")) // POSIX-style short opt (or otherwise ...)
        {
            auto const name = current.substr(1);
            if (Option const* opt = findOption(_context, name)) // -NAME
            {
                consumeToken(_context);
                auto const _optionScope = ScopedOption{_context, *opt};
                return pair{opt, parseValue(_context)};
            }
        }
        else // Natural style option
        {
            auto const name = current;
            if (Option const* opt = findOption(_context, name)) // -NAME
            {
                consumeToken(_context);
                auto const _optionScope = ScopedOption{_context, *opt};
                return pair{opt, parseValue(_context)};
            }
        }

        return nullopt;
    }

    void setOption(ParseContext& _context, string const& _key, Value _value)
    {
        CLI_DEBUG(fmt::format("setOption({}): {}", _key, _value));
        _context.output.values[_key] = move(_value);
   };

    void parseOptionList(ParseContext& _context)
    {
        // Option := Option*
        auto const optionPrefix = namePrefix(_context);

        while (true)
        {
            auto optionOptPair = tryParseOption(_context);
            if (!optionOptPair.has_value())
                break;

            auto& [option, value] = optionOptPair.value();
            auto const fqdn = optionPrefix + "." + Name(option->name);
            setOption(_context, fqdn, move(value));
        }
    }

    auto tryLookupCommand(ParseContext const& _context) -> Command const*
    {
        auto const token = matchPrefix(currentToken(_context), "--")
                         ? currentToken(_context).substr(2)
                         : currentToken(_context);

        for (Command const& command : _context.currentCommand.back()->children)
        {
            if (token == command.name)
                return &command;
        }

        return nullptr; // not found
    }

    auto tryImplicitCommand(ParseContext const& _context) -> Command const*
    {
        for (Command const& command: _context.currentCommand.back()->children)
            if (command.select == CommandSelect::Implicit)
            {
                CLI_DEBUG(fmt::format("Select implicit command {}.", command.name));
                return &command;
            }

        return nullptr;
    }

    void prefillDefaults(ParseContext& _context, Command const& _command)
    {
        auto const _commandScope = ScopedCommand{_context, _command};
        auto const prefix = namePrefix(_context) + ".";

        for (Option const& option : _context.currentCommand.back()->options)
        {
            if (option.presence == Presence::Required)
                continue; // Do not prefill options that are required anyways.

            auto const fqdn = prefix + Name(option.name);
            _context.output.values[fqdn] = option.value;
            setOption(_context, fqdn, option.value);
        }

        for (Command const& _subcmd : _command.children)
        {
            auto const fqdn = prefix + Name(_subcmd.name);
            setOption(_context, fqdn, Value{false});

            prefillDefaults(_context, _subcmd);
        }
    }

    auto parseCommand(Command const& _command, ParseContext& _context) -> bool
    {
        // Command := NAME Option* Section*
        auto const _commandScope = ScopedCommand{_context, _command};
        _context.output.values[namePrefix(_context)] = Value{true};

        parseOptionList(_context);

        if (Command const* subcmd = tryLookupCommand(_context))
        {
            CLI_DEBUG(fmt::format("parseCommand: found sub command: {}", subcmd->name));
            consumeToken(_context); // Name was already ensured to be right (or is assumed to be right).
            parseCommand(*subcmd, _context);
        }
        else if (Command const* subcmd = tryImplicitCommand(_context))
        {
            CLI_DEBUG(fmt::format("parseCommand: found implicit sub command: {}", subcmd->name));
            // DO not consume token
            parseCommand(*subcmd, _context);
        }
        else if (_command.verbatim.has_value())
        {
            CLI_DEBUG(fmt::format("parseCommand: going verbatim."));
            while (_context.pos < _context.args.size())
                _context.output.verbatim.emplace_back(consumeToken(_context));
        }

        if (_context.pos == _context.args.size())
            _context.output.values[namePrefix(_context)] = Value{true};

        // A command must not leave any trailing tokens at the end of parsing
        return _context.pos == _context.args.size();
    }

    StringViewList stringViewList(int argc, char const * const * _argv)
    {
        StringViewList output;
        if (argc <= 0)
            return output;

        output.resize(static_cast<size_t>(argc));

        for (auto const i : times(static_cast<size_t>(argc)))
            output[i] = _argv[i];

        return output;
    }

    void validate(Command const& _command)
    {
        (void) _command;
        // TODO: throw if Command is not well defined.
        //
        // - no duplicated nems in same scope
        // - names must not start with '-' (dash)
        // - must not contain '='
    }

    void validate(Command const& _command, ParseContext& _context, string const& _keyPrefix)
    {
        auto const key = _keyPrefix.empty() ? string(_command.name)
                                            : fmt::format("{}.{}", _keyPrefix, _command.name);

        // Ensure all required fields are provided for those commands that have been provided.
        for (Option const& option: _command.options)
        {
            auto const optionKey = fmt::format("{}.{}", key, option.name);
            if (option.presence == Presence::Required && !_context.output.values.count(optionKey))
                throw invalid_argument(fmt::format("Missing option: {}", optionKey));
        }

        for (Command const& subcmd: _command.children)
        {
            auto const commandKey  = fmt::format("{}.{}", key, subcmd.name);
            if (_context.output.get<bool>(commandKey))
                validate(subcmd, _context, key);
        }
    }

} // }}}

optional<FlagStore> parse(Command const& _command, StringViewList const& _args)
{
    validate(_command);

    auto context = ParseContext{ _args };

    prefillDefaults(context, _command);

    // XXX do not enforce checking the first token, as for main()'s argv[0] this most likely is different.
    // if (currentToken(context) != _command.name)
    //     return nullopt;

    consumeToken(context); // Name was already ensured to be right (or is assumed to be right).
    if (!parseCommand(_command, context))
        return nullopt;

    // auto const& flags = context.output;
    // std::cout << fmt::format("Flags: {}\n", flags.values.size());
    // for (auto const & [k, v] : flags.values)
    //     std::cout << fmt::format(" - {}: {}\n", k, v);

    validate(_command, context, "");

    return move(context.output);
}

optional<FlagStore> parse(Command const& _command, int _argc, char const* const* _argv)
{
    return parse(_command, stringViewList(_argc, _argv));
}

} // }}}
namespace crispy::cli // {{{ Help output
{

namespace // {{{ helpers
{
    string spaces(size_t _count)
    {
        return string(_count, ' ');
    }

    string indent(unsigned _level, unsigned* _cursor = nullptr)
    {
        auto constexpr TabWidth = 4u;

        if (_cursor)
            *_cursor += _level * TabWidth;

        return spaces(_level * TabWidth);
    }

    // TODO: this and OSC-8 (hyperlinks)
    auto stylizer(HelpStyle const& _style) -> function<string(string_view, HelpElement)>
    {
        return [_style](string_view _text, HelpElement _element) -> string {
            auto const [pre, post] = [&]() -> pair<string_view, string_view> {
                if (_style.colors.has_value() && _style.colors.value().count(_element))
                    return {_style.colors.value().at(_element), "\033[m"sv};
                else
                    return {""sv, ""sv};
            }();

            if (!_style.hyperlink)
                return fmt::format("{}{}{}", pre, _text, post);

            string output;
            size_t a = 0;
            for (;;)
            {
                size_t const b = _text.find("://", a);
                if (b == _text.npos || !b)
                    break;

                size_t left = b;
                while (left > 0 && isalpha(_text.at(left - 1)))
                    --left;

                size_t right = b + 3;
                while (right < _text.size() && _text.at(right) != ' ')
                    right++;

                output += pre;
                output += _text.substr(a, left - a);
                output += post;

                output += "\033]8;;";
                output += _text.substr(left, right - left);
                output += "\033\\";

                output += _text.substr(left, right - left);

                output += "\033]8;;\033\\";

                a = right;
            }
            output += pre;
            output += _text.substr(a);
            output += post;

            return output;
        };
    }

    auto colorizer(optional<HelpStyle::ColorMap> const& _colors) -> function<string(string_view, HelpElement)>
    {
        HelpStyle style{};
        style.colors = _colors;
        return stylizer(style);
    }

    string_view wordWrapped(string_view _text, unsigned _margin, unsigned _cursor)
    {
        auto const unwrappedLength = _cursor + _text.size();
        if (unwrappedLength <= _margin)
            return _text;

        // Cut string at right margin, then shift left until we've hit a whitespace character.
        auto i = static_cast<unsigned>(_margin - _cursor + 1);
        while (i > 0 && _text[i] != ' ')
            --i;

        return _text.substr(0, i);
    }

    string wordWrapped(string_view _text, unsigned _indent, unsigned _margin, unsigned* _cursor)
    {
        string output;
        size_t i = 0;
        for (;;)
        {
            while (i < _text.size() && _text[i] == ' ')
                ++i; // skip leading whitespaces

            auto const chunk = wordWrapped(_text.substr(i), _margin, *_cursor);

            output += chunk;
            *_cursor += static_cast<unsigned>(chunk.size());
            i += chunk.size();

            if (i == _text.size())
                break;

            output += '\n';
            output += spaces(_indent);
            *_cursor = _indent + 1;
        }
        return output;
    }

    string printParam(optional<HelpStyle::ColorMap> const& _colors,
                      OptionStyle _optionStyle,
                      string_view _name,
                      string_view _placeholder,
                      Presence _presense)
    {
        auto const colorize = colorizer(_colors);

        stringstream os;

        if (_presense == Presence::Optional)
            os << colorize("[", HelpElement::Braces);
        switch (_optionStyle)
        {
            case OptionStyle::Natural:
                os << colorize(_name, HelpElement::OptionName);
                if (!_placeholder.empty())
                    os << ' ' << colorize(_placeholder, HelpElement::OptionValue);
                break;
            case OptionStyle::Posix:
                os << colorize("--", HelpElement::OptionDash) << colorize(_name, HelpElement::OptionName);
                if (!_placeholder.empty())
                    os << colorize("=", HelpElement::OptionEqual) << colorize(_placeholder, HelpElement::OptionValue);
                break;
        }
        if (_presense == Presence::Optional)
            os << colorize("]", HelpElement::Braces);

        return os.str();
    }

    string printOption(Option const& _option,
                       optional<HelpStyle::ColorMap> const& _colors,
                       OptionStyle _optionStyle)
    {
        // TODO: make use of _option.placeholder
        auto const placeholder = [](Option const& _option, string_view _type) -> string_view
        {
            return !_option.placeholder.empty() ? _option.placeholder : _type;
        };

        if (holds_alternative<bool>(_option.value))
            return printParam(_colors, _optionStyle, _option.name, placeholder(_option, ""), _option.presence);
        else if (holds_alternative<int>(_option.value))
            return printParam(_colors, _optionStyle, _option.name, placeholder(_option, "INT"), _option.presence);
        else if (holds_alternative<unsigned int>(_option.value))
            return printParam(_colors, _optionStyle, _option.name, placeholder(_option, "UINT"), _option.presence);
        else if (holds_alternative<double>(_option.value))
            return printParam(_colors, _optionStyle, _option.name, placeholder(_option, "FLOAT"), _option.presence);
        else
            return printParam(_colors, _optionStyle, _option.name, placeholder(_option, "STRING"), _option.presence);
    }

    string printOption(Option const& _option,
                       optional<HelpStyle::ColorMap> const& _colors,
                       OptionStyle _displayStyle,
                       unsigned _indent, unsigned _margin, unsigned* _cursor)
    {
        auto const plainTextLength = static_cast<unsigned>(printOption(_option, nullopt, _displayStyle).size());
        if (*_cursor + plainTextLength < _margin)
        {
            *_cursor += plainTextLength;
            return printOption(_option, _colors, _displayStyle);
        }
        else
        {
            *_cursor = _indent + 1 + plainTextLength;
            return "\n" + spaces(_indent) + printOption(_option, _colors, _displayStyle);
        }
    }

    size_t longestOptionText(OptionList const& _options, OptionStyle _displayStyle)
    {
        size_t result = 0;
        for (Option const& option : _options)
            result = max(result, printOption(option, nullopt, _displayStyle).size());
        return result;
    }

    void detailedDescription(ostream& _os, Command const& _command,
                             HelpStyle const& _style, unsigned _margin, vector<Command const*>& _parents)
    {
        // NOTE: We asume that cursor position is at first column!
        auto const stylize = stylizer(_style);
        bool const hasParentCommand = !_parents.empty();
        bool const isLeafCommand = _command.children.empty();

        if (isLeafCommand || !_command.options.empty() || _command.verbatim.has_value()) // {{{ print command sequence
        {
            _os << indent(1);
            for (Command const* parent : _parents)
                _os << stylize(parent->name, HelpElement::OptionValue/*well, yeah*/) << ' ';

            if (_command.select == CommandSelect::Explicit)
                _os << _command.name;
            else
            {
                _os << stylize("[", HelpElement::Braces);
                _os << stylize(_command.name, HelpElement::ImplicitCommand);
                _os << stylize("]", HelpElement::Braces);
            }

            _os << "\n";

            if (hasParentCommand)
            {
                unsigned cursor = 1;
                _os << indent(2, &cursor);
                _os << stylize(wordWrapped(_command.helpText, cursor, _margin, &cursor), HelpElement::HelpText) << "\n\n";
            }
        }
        // }}}
        if (!_command.options.empty() || _command.verbatim.has_value()) // {{{ print options
        {
            _os << indent(2) << stylize("Options:", HelpElement::Header) << "\n\n";

            auto const leftPadding = indent(3);
            auto const minRightPadSize = 2;
            auto const maxOptionTextSize = longestOptionText(_command.options, _style.optionStyle);
            auto const columnWidth = static_cast<unsigned>(leftPadding.size() + maxOptionTextSize + minRightPadSize);

            for (Option const& option : _command.options)
            {
                auto const leftSize = static_cast<unsigned>(leftPadding.size() + printOption(option, nullopt, _style.optionStyle).size());
                auto const actualRightPaddingSize = columnWidth - leftSize;
                auto const left = leftPadding + printOption(option, _style.colors, _style.optionStyle) + spaces(actualRightPaddingSize);

                _os << left;

                auto cursor = static_cast<unsigned>(columnWidth + 1);
                _os << stylize(wordWrapped(option.helpText, columnWidth, _margin, &cursor), HelpElement::HelpText);

                // {{{ append default value, if any
                auto const defaultValueStr = fmt::format("{}", option.value);
                if ((option.presence == Presence::Optional && !defaultValueStr.empty())
                    || (holds_alternative<bool>(option.value) && get<bool>(option.value)))
                {
                    auto const DefaultTextPrefix = string("default:");
                    auto const defaultText = stylize("[", HelpElement::Braces)
                                           + DefaultTextPrefix + " "
                                           + stylize(defaultValueStr, HelpElement::OptionValue)
                                           + stylize("]", HelpElement::Braces);
                    auto const defaultTextLength = static_cast<unsigned>(1 + DefaultTextPrefix.size() + 1 + defaultValueStr.size() + 1);
                    if (cursor + defaultTextLength > _margin)
                        _os << "\n" << spaces(columnWidth) << defaultText;
                    else
                        _os << " " << defaultText;
                }
                // }}}

                _os << '\n';
            }
            if (_command.verbatim.has_value())
            {
                auto const& verbatim = _command.verbatim.value();
                auto const leftSize = leftPadding.size() + 2 + verbatim.placeholder.size();
                auto const actualRightPaddingSize = static_cast<unsigned>(columnWidth - leftSize);
                auto const left = leftPadding
                    + stylize("[", HelpElement::Braces)
                    + stylize(verbatim.placeholder, HelpElement::Verbatim)
                    + stylize("]", HelpElement::Braces)
                    + spaces(actualRightPaddingSize);

                _os << left;
                auto cursor = static_cast<unsigned>(columnWidth + 1);
                _os << stylize(wordWrapped(verbatim.helpText, columnWidth, _margin, &cursor), HelpElement::HelpText);
                _os << '\n';
            }
            _os << '\n';
        }
        // }}}
        if (!_command.children.empty()) // {{{ recurse to sub commands
        {
            _parents.emplace_back(&_command);
            for (Command const& subcmd : _command.children)
                detailedDescription(_os, subcmd, _style, _margin, _parents);
            _parents.pop_back();
        } // }}}
    }

    void detailedDescription(ostream& _os, Command const& _command, HelpStyle const& _style, unsigned _margin)
    {
        vector<Command const*> parents;
        detailedDescription(_os, _command, _style, _margin, parents);
    }
} // }}}

HelpStyle::ColorMap HelpStyle::defaultColors()
{
    return ColorMap{
        {HelpElement::Header, "\033[32;1;4:2m"},
        {HelpElement::Braces, "\033[37;1m"},
        {HelpElement::OptionDash, "\033[34;1m"},
        {HelpElement::OptionName, "\033[37m"},
        {HelpElement::OptionEqual, "\033[34;1m"},
        {HelpElement::OptionValue, "\033[36m"},
        {HelpElement::ImplicitCommand, "\033[33;1m"},
        {HelpElement::Verbatim, "\033[36m"},
        {HelpElement::HelpText, "\033[38m"},
    };
}

/**
 * Constructs a usage text suitable for printing out the command usage syntax in terminals.
 *
 * @param _command The command to construct the usage text for.
 * @param _colored Boolean indicating whether or not to colorize the output via VT sequences.
 * @param _margin  Number of characters to write at most per line.
 */
string usageText(Command const& _command, HelpStyle const& _style, unsigned _margin, string const& _cmdPrefix)
{
    auto const colorize = colorizer(_style.colors);
    auto const indentationWidth = static_cast<unsigned>(_cmdPrefix.size());

    auto const printOptionList = [&](ostream& _os, OptionList const& _options, unsigned* _cursor)
    {
        auto const indent = *_cursor;
        for (Option const& option : _options)
            _os << ' ' << printOption(option, _style.colors, _style.optionStyle,
                                      indent, _margin, _cursor);
    };

    auto cursor = indentationWidth + 1;
    if (_command.children.empty())
    {
        stringstream sstr;
        sstr << _cmdPrefix;

        if (_command.select == CommandSelect::Explicit)
        {
            cursor += static_cast<unsigned>(_command.name.size());
            sstr << _command.name;
        }
        else
        {
            cursor += static_cast<unsigned>(_command.name.size() + 2);
            sstr << colorize("[", HelpElement::Braces);
            sstr << colorize(_command.name, HelpElement::ImplicitCommand);
            sstr << colorize("]", HelpElement::Braces);
        }

        auto const indent = cursor;
        printOptionList(sstr, _command.options, &cursor);

        if (_command.verbatim.has_value())
        {
            if (cursor + 3 + _command.verbatim.value().placeholder.size() > size_t(_margin))
            {
                sstr << "\n";
                sstr << spaces(indent);
            }
            else
                sstr << ' ';

            sstr << colorize("[", HelpElement::Braces);
            sstr << colorize(_command.verbatim.value().placeholder, HelpElement::Verbatim);
            sstr << colorize("]", HelpElement::Braces);
        }

        sstr << '\n';
        return sstr.str();
    }
    else
    {
        stringstream prefix;
        prefix << _cmdPrefix << _command.name;
        printOptionList(prefix, _command.options, &cursor);
        prefix << ' ';

        string const prefixStr = prefix.str();
        stringstream sstr;
        for (Command const& subcmd : _command.children)
            sstr << usageText(subcmd, _style, _margin, prefixStr);
        if (_command.children.empty())
            sstr << '\n';
        return sstr.str();
    }
}

string helpText(Command const& _command, HelpStyle const& _style, unsigned _margin)
{
    auto const stylize = stylizer(_style);

    stringstream output;

    output << stylize(_command.helpText, HelpElement::HelpText) << "\n\n";

    output << "  " << stylize("Usage:", HelpElement::Header) << "\n\n";
    output << usageText(_command, _style, _margin, indent(1));
    output << '\n';

    auto constexpr DescriptionHeader = string_view{"Detailed description:"};

    output << "  " << stylize(DescriptionHeader, HelpElement::Header) << "\n\n";
    detailedDescription(output, _command, _style, _margin);

    return output.str();
}

} // }}}
