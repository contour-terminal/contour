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
#pragma once

#include <terminal/InputGenerator.h>
#include <terminal/Logger.h>

#include <iostream>
#include <string>

enum class LogMask {
    None  = 0,

    ParserError         = 0x01,
    RawInput            = 0x02,
    RawOutput           = 0x04,
    InvalidOutput       = 0x08,
    UnsupportedOutput   = 0x10,
    TraceOutput         = 0x20,
    TraceInput          = 0x40,
};

constexpr LogMask operator&(LogMask lhs, LogMask rhs) noexcept
{
    return static_cast<LogMask>(static_cast<unsigned>(lhs) & static_cast<unsigned>(rhs));
}

constexpr LogMask operator|(LogMask lhs, LogMask rhs) noexcept
{
    return static_cast<LogMask>(static_cast<unsigned>(lhs) | static_cast<unsigned>(rhs));
}

constexpr LogMask& operator|=(LogMask& lhs, LogMask rhs) noexcept
{
    lhs = lhs | rhs;
    return lhs;
}

constexpr bool operator!=(LogMask lhs, unsigned rhs) noexcept
{
    return static_cast<unsigned>(lhs) != rhs;
}

/// glterm Logging endpoint.
class GLLogger {
  public:
    GLLogger(LogMask _mask, std::ostream* _sink);
    GLLogger() : GLLogger{LogMask::ParserError | LogMask::InvalidOutput | LogMask::UnsupportedOutput, nullptr} {}

    LogMask logMask() const noexcept { return logMask_; }
    void setLogMask(LogMask _level) { logMask_ = _level; }

    void log(terminal::LogEvent const& _event);
    void operator()(terminal::LogEvent const& _event) { log(_event); }

    std::ostream* sink() noexcept { return sink_; }

    // debugging endpoints
    void keyPress(terminal::Key _key, terminal::Modifier _modifier);
    void keyPress(char32_t _char, terminal::Modifier _modifier);

    // trace endpoints
    void keyTrace(std::string const& _message);

    void flush();

  private:
    LogMask logMask_;
    std::ostream* sink_;
};
