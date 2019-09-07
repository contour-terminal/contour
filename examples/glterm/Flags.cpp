/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019 Christian Parpart <christian@parpart.family>
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
#include "Flags.h"

#include <array>
#include <iomanip>
#include <iostream>
#include <sstream>

using namespace std;

namespace AnsiColor {
    enum Code : unsigned {
        Clear = 0,
        Reset = Clear,
        Bold = 0x0001,  // 1
        Dark = 0x0002,  // 2
        Undef1 = 0x0004,
        Underline = 0x0008,  // 4
        Blink = 0x0010,      // 5
        Undef2 = 0x0020,
        Reverse = 0x0040,    // 7
        Concealed = 0x0080,  // 8
        AllFlags = 0x00FF,
        Black = 0x0100,
        Red = 0x0200,
        Green = 0x0300,
        Yellow = 0x0400,
        Blue = 0x0500,
        Magenta = 0x0600,
        Cyan = 0x0700,
        White = 0x0800,
        AnyFg = 0x0F00,
        OnBlack = 0x1000,
        OnRed = 0x2000,
        OnGreen = 0x3000,
        OnYellow = 0x4000,
        OnBlue = 0x5000,
        OnMagenta = 0x6000,
        OnCyan = 0x7000,
        OnWhite = 0x8000,
        AnyBg = 0xF000
    };

    /// Combines two ANSI escape sequences into one Code.
    constexpr inline Code operator|(Code a, Code b)
    {
        return Code{unsigned(a) | unsigned(b)};
    }

    /**
     * Counts the number of ANSI escape sequences in @p codes.
     */
    constexpr unsigned count(Code codes)
    {
        if (codes == Clear)
            return 1;

        unsigned i = 0;

        if (codes & AllFlags)
            for (int k = 0; k < 8; ++k)
                if (codes & (1 << k))
                    ++i;

        if (codes & AnyFg)
            ++i;

        if (codes & AnyBg)
            ++i;

        return i;
    }

    /**
     * Retrieves the number of bytes required to store the ANSI escape sequences of @p codes
     * without prefix/suffix notation.
     */
    constexpr unsigned capacity(Code codes)
    {
        if (codes == Clear)
            return 1;

        unsigned i = 0;

        if (codes & AllFlags)
            for (int k = 0; k < 8; ++k)
                if (codes & (1 << k))
                    ++i;

        if (codes & AnyFg)
            i += 2;

        if (codes & AnyBg)
            i += 2;

        return i + (count(codes) - 1);
    }

    /// Constructs a sequence of ANSI codes for the colors in this @p codes.
    template <const Code value, const bool EOS = true>
    constexpr auto codes()
    {
        std::array<char, capacity(value) + 3 + (EOS ? 1 : 0)> result{};

        size_t n = 0;  // n'th escape sequence being iterate through
        size_t i = 0;  // i'th byte in output array

        result[i++] = '\x1B';
        result[i++] = '[';

        if constexpr (value != 0)
        {
            if (value & AllFlags)
            {
                for (int k = 0; k < 8; ++k)
                {
                    if (value & (1 << k))
                    {
                        if (n++)
                            result[i++] = ';';
                        result[i++] = k + '1';
                    }
                }
            }

            if (value & AnyFg)
            {
                if (n++)
                    result[i++] = ';';
                unsigned const val = ((value >> 8) & 0x0F) + 29;  // 36 -> {'3', '6'}
                result[i++] = (val / 10) + '0';
                result[i++] = (val % 10) + '0';
            }

            if (value & AnyBg)
            {
                if (n++)
                    result[i++] = ';';
                unsigned const val = ((value >> 12) & 0x0F) + 39;
                result[i++] = (val / 10) + '0';
                result[i++] = (val % 10) + '0';
            }
        }
        else
            result[i++] = '0';  // reset/clear

        result[i++] = 'm';

        return result;
    }

}  // namespace AnsiColor

namespace util {

auto static constexpr clearColor = AnsiColor::codes<AnsiColor::Clear>();
auto static constexpr optionColor = AnsiColor::codes<AnsiColor::Bold | AnsiColor::Cyan>();
auto static constexpr valueColor = AnsiColor::codes<AnsiColor::Bold | AnsiColor::Red>();
auto static constexpr headerColor = AnsiColor::codes<AnsiColor::Bold | AnsiColor::Green>();

// {{{ Flags::Error
Flags::Error::Error(ErrorCode code, string arg)
    : runtime_error{FlagsErrorCategory::get().message(static_cast<int>(code)) + ": " + arg},
      code_{code}, arg_{move(arg)}
{
}
// }}}

// {{{ Flag
Flags::Flag::Flag(const string& opt, const string& val, FlagStyle fs, FlagType ft)
    : type_(ft), style_(fs), name_(opt), value_(val)
{
}
// }}}

Flags::Flags()
    : flagDefs_{}, parametersEnabled_{false}, parametersPlaceholder_{}, parametersHelpText_{}, set_{}, raw_{}
{
}

void Flags::set(const Flag& flag)
{
    set_[flag.name()] = make_pair(flag.type(), flag.value());
}

void Flags::set(const string& opt, const string& val, FlagStyle fs, FlagType ft)
{
    set(Flag{opt, val, fs, ft});
}

bool Flags::isSet(const string& flag) const
{
    return set_.find(flag) != set_.end();
}

string Flags::asString(const string& flag) const
{
    auto i = set_.find(flag);
    if (i == set_.end())
        throw Error{ErrorCode::NotFound, flag};

    return i->second.second;
}

string Flags::getString(const string& flag) const
{
    auto i = set_.find(flag);
    if (i == set_.end())
        throw Error{ErrorCode::NotFound, flag};

    if (i->second.first != FlagType::String)
        throw Error{ErrorCode::TypeMismatch, flag};

    return i->second.second;
}

long int Flags::getNumber(const string& flag) const
{
    auto i = set_.find(flag);
    if (i == set_.end())
        throw Error{ErrorCode::NotFound, flag};

    if (i->second.first != FlagType::Number)
        throw Error{ErrorCode::TypeMismatch, flag};

    return stoi(i->second.second);
}

float Flags::getFloat(const string& flag) const
{
    auto i = set_.find(flag);
    if (i == set_.end())
        throw Error{ErrorCode::NotFound, flag};

    if (i->second.first != FlagType::Float)
        throw Error{ErrorCode::TypeMismatch, flag};

    return stof(i->second.second);
}

bool Flags::getBool(const string& flag) const
{
    auto i = set_.find(flag);
    if (i == set_.end())
        return false;

    return i->second.second == "true";
}

const vector<string>& Flags::parameters() const
{
    return raw_;
}

void Flags::setParameters(const vector<string>& v)
{
    raw_ = v;
}

string Flags::to_s() const
{
    stringstream sstr;

    int i = 0;
    for (const pair<string, FlagValue>& flag : set_)
    {
        if (i)
            sstr << ' ';

        i++;

        switch (flag.second.first)
        {
            case FlagType::Bool:
                if (flag.second.second == "true")
                    sstr << "--" << flag.first;
                else
                    sstr << "--" << flag.first << "=false";
                break;
            case FlagType::String:
                sstr << "--" << flag.first << "=\"" << flag.second.second << "\"";
                break;
            default:
                sstr << "--" << flag.first << "=" << flag.second.second;
                break;
        }
    }

    return sstr.str();
}

Flags& Flags::define(const string& longOpt, char shortOpt, bool required, FlagType type,
                     const string& valuePlaceholder, const string& helpText,
                     const optional<string>& defaultValue, function<void(const string&)> callback)
{
    FlagDef fd;
    fd.type = type;
    fd.longOption = longOpt;
    fd.shortOption = shortOpt;
    fd.required = required;
    fd.valuePlaceholder = valuePlaceholder;
    fd.helpText = helpText;
    fd.defaultValue = defaultValue;
    fd.callback = callback;

    flagDefs_.emplace_back(fd);

    return *this;
}

Flags& Flags::defineString(const string& longOpt, char shortOpt, const string& valuePlaceholder,
                           const string& helpText, optional<string> defaultValue,
                           function<void(const string&)> callback)
{
    return define(longOpt, shortOpt, false, FlagType::String, valuePlaceholder, helpText, defaultValue,
                  callback);
}

Flags& Flags::defineNumber(const string& longOpt, char shortOpt, const string& valuePlaceholder,
                           const string& helpText, optional<long int> defaultValue,
                           function<void(long int)> callback)
{
    return define(longOpt, shortOpt, false, FlagType::Number, valuePlaceholder, helpText,
                  defaultValue.has_value() ? make_optional(to_string(*defaultValue)) : nullopt,
                  [=](const string& value) {
                      if (callback)
                      {
                          callback(stoi(value));
                      }
                  });
}

Flags& Flags::defineFloat(const string& longOpt, char shortOpt, const string& valuePlaceholder,
                          const string& helpText, optional<float> defaultValue,
                          function<void(float)> callback)
{
    return define(longOpt, shortOpt, false, FlagType::Float, valuePlaceholder, helpText,
                  defaultValue.has_value() ? make_optional(to_string(*defaultValue)) : nullopt,
                  [=](const string& value) {
                      if (callback)
                      {
                          callback(stof(value));
                      }
                  });
}

Flags& Flags::defineBool(const string& longOpt, char shortOpt, const string& helpText,
                         function<void(bool)> callback)
{
    return define(longOpt, shortOpt, false, FlagType::Bool, "<bool>", helpText, nullopt,
                  [=](const string& value) {
                      if (callback)
                      {
                          callback(value == "true");
                      }
                  });
}

Flags& Flags::enableParameters(const string& valuePlaceholder, const string& helpText)
{
    parametersEnabled_ = true;
    parametersPlaceholder_ = valuePlaceholder;
    parametersHelpText_ = helpText;

    return *this;
}

const Flags::FlagDef* Flags::findDef(const string& longOption) const
{
    for (const auto& flag : flagDefs_)
        if (flag.longOption == longOption)
            return &flag;

    return nullptr;
}

const Flags::FlagDef* Flags::findDef(char shortOption) const
{
    for (const auto& flag : flagDefs_)
        if (flag.shortOption == shortOption)
            return &flag;

    return nullptr;
}

// -----------------------------------------------------------------------------
void Flags::parse(int argc, const char* argv[])
{
    vector<string> args;
    for (int i = 1; i < argc; ++i)
        args.push_back(argv[i]);

    parse(args);
}

error_code Flags::tryParse(const vector<string>& args)
{
    try
    {
        parse(args);
    }
    catch (const Error& parseError)
    {
        return parseError.code();
    }
    return error_code();
}

void Flags::parse(const vector<string>& args)
{
    auto invokeCallback = [&](const FlagDef* fd, FlagStyle style, const string& value) {
        if (fd)
        {
            set(fd->longOption, value, style, fd->type);
            if (fd->callback)
            {
                fd->callback(value);
            }
        }
    };

    enum class ParsingState {
        Options,
        Parameters,
    };

    vector<string> params;
    ParsingState pstate = ParsingState::Options;
    size_t i = 0;

    while (i < args.size())
    {
        string arg = args[i];
        i++;
        if (pstate == ParsingState::Parameters)
            params.push_back(arg);
        else if (arg == "--")
        {
            if (parametersEnabled_)
                pstate = ParsingState::Parameters;
            else
                throw Error{ErrorCode::UnknownOption, arg};
        }
        else if (arg.size() > 2 && arg[0] == '-' && arg[1] == '-')
        {
            // longopt
            string name = arg.substr(2);
            size_t eq = name.find('=');
            if (eq != name.npos)
            {  // --name=value
                string value = name.substr(eq + 1);
                name = name.substr(0, eq);
                const FlagDef* fd = findDef(name);
                if (fd == nullptr)
                    throw Error{ErrorCode::UnknownOption, arg};
                else
                    invokeCallback(fd, FlagStyle::LongWithValue, value);
            }
            else
            {  // --name [VALUE]
                const FlagDef* fd = findDef(name);
                if (fd == nullptr)
                    throw Error{ErrorCode::UnknownOption, arg};
                else if (fd->type == FlagType::Bool)
                    // --name
                    invokeCallback(fd, FlagStyle::LongSwitch, "true");
                else
                {
                    // --name VALUE
                    if (i >= args.size())
                        throw Error{ErrorCode::MissingOption, arg};

                    string value = args[i];
                    i++;

                    invokeCallback(fd, FlagStyle::LongWithValue, value);
                }
            }
        }
        else if (arg.size() > 1 && arg[0] == '-')
        {
            // shortopt
            arg = arg.substr(1);
            while (!arg.empty())
            {
                const FlagDef* fd = findDef(arg[0]);
                if (fd == nullptr)  // option not found
                    throw Error{ErrorCode::UnknownOption, "-" + arg.substr(0, 1)};
                else if (fd->type == FlagType::Bool)
                {
                    invokeCallback(fd, FlagStyle::ShortSwitch, "true");
                    arg = arg.substr(1);
                }
                else if (arg.size() > 1)  // -fVALUE
                {
                    string value = arg.substr(1);
                    invokeCallback(fd, FlagStyle::ShortSwitch, value);
                    arg.clear();
                }
                else
                {
                    // -f VALUE
                    string name = fd->longOption;

                    if (i >= args.size())
                    {
                        char option[3] = {'-', fd->shortOption, '\0'};
                        throw Error{ErrorCode::MissingOptionValue, option};
                    }

                    arg.clear();
                    string value = args[i];
                    i++;

                    if (!value.empty() && value[0] == '-')
                    {
                        char option[3] = {'-', fd->shortOption, '\0'};
                        throw Error{ErrorCode::MissingOptionValue, option};
                    }

                    invokeCallback(fd, FlagStyle::ShortSwitch, value);
                }
            }
        }
        else if (parametersEnabled_)
            params.push_back(arg);
        else
            throw Error{ErrorCode::UnknownOption, arg};
    }

    setParameters(params);

    // fill any missing default flags
    for (const FlagDef& fd : flagDefs_)
    {
        if (fd.defaultValue.has_value())
        {
            if (!isSet(fd.longOption))
                invokeCallback(&fd, FlagStyle::LongWithValue, fd.defaultValue.value());
        }
        else if (fd.type == FlagType::Bool)
        {
            if (!isSet(fd.longOption))
                invokeCallback(&fd, FlagStyle::LongWithValue, "false");
        }
    }
}

// -----------------------------------------------------------------------------

string Flags::helpText(string_view const& header, size_t width, size_t helpTextOffset) const
{
    stringstream sstr;

    if (!header.empty())
        sstr << headerColor.data() << header << clearColor.data();

    if (parametersEnabled_ || !flagDefs_.empty())
        sstr << headerColor.data() << "Options:\n" << clearColor.data();

    for (const FlagDef& fd : flagDefs_)
        sstr << fd.makeHelpText(width, helpTextOffset);

    if (parametersEnabled_)
    {
        sstr << endl;

        const streampos p = sstr.tellp();
        const size_t column = static_cast<size_t>(sstr.tellp() - p);

        sstr << "    [--] " << valueColor.data() << parametersPlaceholder_ << clearColor.data();
        if (column < helpTextOffset)
            sstr << setw(helpTextOffset - column) << ' ';
        else
            sstr << endl << setw(helpTextOffset) << ' ';

        sstr << parametersHelpText_ << endl;
    }

    return sstr.str();
}

static string wordWrap(const string& text, size_t currentWidth, size_t width, size_t indent)
{
    stringstream sstr;

    size_t i = 0;
    while (i < text.size())
    {
        if (currentWidth >= width)
        {
            sstr << endl << setw(indent) << ' ';
            currentWidth = 0;
        }

        sstr << text[i];
        currentWidth++;
        i++;
    }

    return sstr.str();
}

error_code make_error_code(Flags::ErrorCode errc)
{
    return error_code(static_cast<int>(errc), FlagsErrorCategory::get());
}

// {{{ Flags::FlagDef
string Flags::FlagDef::makeHelpText(size_t width, size_t helpTextOffset) const
{
    stringstream sstr;

    sstr << "  ";

    // short option
    if (shortOption)
        sstr << optionColor.data() << "-" << shortOption << clearColor.data() << ", ";
    else
        sstr << "    ";

    // long option
    sstr << optionColor.data() << "--" << longOption;

    // value placeholder
    if (type != FlagType::Bool)
    {
        sstr << "=" << valueColor.data();
        if (!valuePlaceholder.empty())
            sstr << valuePlaceholder;
        else
            sstr << "VALUE";
    }
    sstr << clearColor.data();

    // spacer
    size_t column = static_cast<size_t>(sstr.tellp());
    if (column < helpTextOffset)
        sstr << setw(helpTextOffset - sstr.tellp()) << ' ';
    else
    {
        sstr << endl << setw(helpTextOffset) << ' ';
        column = helpTextOffset;
    }

    // help output with default value hint.
    if (type != FlagType::Bool && defaultValue.has_value())
        sstr << wordWrap(helpText + " [" + *defaultValue + "]", column, width, helpTextOffset);
    else
        sstr << wordWrap(helpText, column, width, helpTextOffset);

    sstr << endl;

    return sstr.str();
}
// }}}

// {{{ FlagsErrorCategory
FlagsErrorCategory& FlagsErrorCategory::get()
{
    static FlagsErrorCategory cat;
    return cat;
}

const char* FlagsErrorCategory::name() const noexcept
{
    return "Flags";
}

string FlagsErrorCategory::message(int ec) const
{
    switch (static_cast<Flags::ErrorCode>(ec))
    {
        case Flags::ErrorCode::TypeMismatch:
            return "Type Mismatch";
        case Flags::ErrorCode::UnknownOption:
            return "Unknown Option";
        case Flags::ErrorCode::MissingOption:
            return "Missing Option";
        case Flags::ErrorCode::MissingOptionValue:
            return "Missing Option Value";
        case Flags::ErrorCode::NotFound:
            return "Flag Not Found";
        default:
            return "<UNKNOWN>";
    }
}
// }}}

}  // namespace util

