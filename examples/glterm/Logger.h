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

#include <iostream>
#include <string>

enum class LogLevel {
    None = 0,
    Error = 1,
    Warning = 2,
    Debug = 3,
    Trace = 4
};

/// glterm Logging endpoint.
class Logger {
  public:
    Logger(LogLevel _logLevel, std::ostream* _sink);
    Logger() : Logger{LogLevel::None, nullptr} {}

    LogLevel logLevel() const noexcept { return logLevel_; }
    void setLogLevel(LogLevel _level) { logLevel_ = _level; }

    std::ostream* sink() noexcept { return sink_; }

    // error endpoint (e.g. invalid CSI)
    void error(std::string const& _message);

    // warning endpoint (e.g. unsupported CSI)
    void warning(std::string const& _message);

    // debugging endpoints
    void debug(std::string const& _message);
    void keyPress(terminal::Key _key, terminal::Modifier _modifier);
    void keyPress(char32_t _char, terminal::Modifier _modifier);

    // trace endpoints
    void keyTrace(std::string const& _message);
    void screenTrace(std::string const& _message);

    void flush();

  private:
    void log(LogLevel _logLevel, std::string const& _message);

  private:
    LogLevel logLevel_;
    std::ostream* sink_;
};
