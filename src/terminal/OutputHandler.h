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
#pragma once

#include <terminal/Logger.h>
#include <terminal/Parser.h>
#include <terminal/Functions.h>

#include <string>
#include <string_view>
#include <vector>

namespace terminal {

class OutputHandler {
  public:
    using ActionClass = Parser::ActionClass;
    using Action = Parser::Action;

    explicit OutputHandler(Logger _logger);

    static std::optional<RGBColor> parseColor(std::string_view const& _value);

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
    void dispatchOSC();

    template <typename Event, typename... Args>
    void log(Args&&... args) const
    {
        if (logger_)
            logger_(Event{ std::forward<Args>(args)... });
    }

    void logInvalidESC(std::string const& message = "") const;
    void logInvalidCSI(std::string const& message = "") const;
    void logUnsupportedCSI() const;

    template <typename T, typename... Args>
    ApplyResult emitCommand(Args&&... args)
    {
        commands_.emplace_back(T{std::forward<Args>(args)...});
        // TODO: telemetry_.increment(sequence_);
        return ApplyResult::Ok;
    }

  private:
    Sequence sequence_{};
    CommandList commands_{};

    Logger const logger_;
};

}  // namespace terminal
