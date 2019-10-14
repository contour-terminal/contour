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

#include <terminal/Logger.h>
#include <terminal/Parser.h>
#include <terminal/FunctionDef.h>

#include <string>
#include <string_view>
#include <vector>

namespace terminal {

class OutputHandler : private HandlerContext {
  public:
    using ActionClass = Parser::ActionClass;
    using Action = Parser::Action;

    explicit OutputHandler(Logger _logger);

    void invokeAction(ActionClass _actionClass, Action _action, char32_t _finalChar);

    void operator()(ActionClass _actionClass, Action _action, char32_t _finalChar)
    {
        return invokeAction(_actionClass, _action, _finalChar);
    }

    std::vector<Command>& commands() noexcept { return commands_; }
    std::vector<Command> const& commands() const noexcept { return commands_; }

  private:
    void executeControlFunction(char _c0);
    void dispatchESC(char _finalChar);
    void dispatchCSI(char _finalChar);

    template <typename Event, typename... Args>
    void log(std::string_view const& msg, Args&&... args) const
    {
        if (logger_)
            logger_(Event{ fmt::format(msg, std::forward<Args>(args)...) });
    }

    void logInvalidESC(char _finalChar, std::string const& message = "") const;
    void logInvalidCSI(char _finalChar, std::string const& message = "") const;
    void logUnsupportedCSI(char _finalChar) const;
    void logUnsupported(std::string_view const& msg) const;

    template <typename... Args>
    void logUnsupported(std::string_view const& msg, Args... args) const
    {
        logUnsupported(fmt::format(msg, std::forward<Args>(args)...));
    }

    std::string sequenceString(char _finalChar, std::string const& _prefix) const;

  private:
    char32_t currentChar_{};

	char leaderSymbol_ = 0;
    bool private_ = false;

    Logger const logger_;

	FunctionHandlerMap functionMapper_;
};

}  // namespace terminal
