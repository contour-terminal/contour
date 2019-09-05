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
#include "Logger.h"

#include <terminal/UTF8.h>
#include <terminal/Util.h>

#include <fmt/format.h>

#include <array>

using namespace std;
using namespace terminal;

Logger::Logger(LogLevel _logLevel, std::ostream* _sink) :
    logLevel_{ _logLevel },
    sink_{ _sink }
{
}

void Logger::error(string const& _message)
{
    log(LogLevel::Error, _message);
}

void Logger::warning(string const& _message)
{
    log(LogLevel::Warning, _message);
}

void Logger::debug(string const& _message)
{
    log(LogLevel::Debug, _message);
}

void Logger::keyPress(Key _key, Modifier _modifier)
{
    log(LogLevel::Debug, fmt::format("key key: {} {}", to_string(_key), to_string(_modifier)));
}

void Logger::keyPress(char32_t _char, Modifier _modifier)
{
    if (utf8::isASCII(_char) && isprint(_char))
        log(LogLevel::Debug, fmt::format("char: {} ({})", static_cast<char>(_char), to_string(_modifier)));
    else
        log(LogLevel::Debug, fmt::format("char: 0x{:04X} ({})", static_cast<uint32_t>(_char), to_string(_modifier)));
}

void Logger::keyTrace(string const& _message)
{
    log(LogLevel::Trace, _message);
}

void Logger::screenTrace(string const& _message)
{
    log(LogLevel::Trace, _message);
}

void Logger::log(LogLevel _logLevel, string const& _message)
{
    auto const static names = array{
        "none",
        "error",
        "warning",
        "debug",
        "trace"
    };
    if (_logLevel <= logLevel_ && sink_)
        *sink_ << fmt::format("[{}] {}\n", names.at(static_cast<size_t>(_logLevel)), _message);
}

void Logger::flush()
{
    if (sink_)
        sink_->flush();
}
