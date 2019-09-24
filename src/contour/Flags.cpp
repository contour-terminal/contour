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

namespace util {

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
        sstr << header;

    if (parametersEnabled_ || !flagDefs_.empty())
        sstr << "Options:\n";

    for (const FlagDef& fd : flagDefs_)
        sstr << fd.makeHelpText(width, helpTextOffset);

    if (parametersEnabled_)
    {
        sstr << endl;

        const streampos p = sstr.tellp();
        const size_t column = static_cast<size_t>(sstr.tellp() - p);

        sstr << "    [--] " << parametersPlaceholder_;
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
        sstr << "-" << shortOption << ", ";
    else
        sstr << "    ";

    // long option
    sstr << "--" << longOption;

    // value placeholder
    if (type != FlagType::Bool)
    {
        sstr << "=";
        if (!valuePlaceholder.empty())
            sstr << valuePlaceholder;
        else
            sstr << "VALUE";
    }

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

