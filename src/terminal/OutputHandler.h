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

#include <string>
#include <string_view>
#include <vector>

namespace terminal {

class OutputHandler {
  public:
    using ActionClass = Parser::ActionClass;
    using Action = Parser::Action;

    size_t constexpr static MaxParameters = 16;

    OutputHandler(unsigned int _rows, Logger _logger)
        : rowCount_{_rows},
          logger_{std::move(_logger)}
    {
        parameters_.reserve(MaxParameters);
    }

    void updateRowCount(unsigned int rows)
    {
        rowCount_ = rows;
    }

    void invokeAction(ActionClass actionClass, Action action, char32_t currentChar);

    void operator()(ActionClass actionClass, Action action, char32_t currentChar)
    {
        return invokeAction(actionClass, action, currentChar);
    }

    std::vector<Command>& commands() noexcept { return commands_; }
    std::vector<Command> const& commands() const noexcept { return commands_; }

  private:
    char32_t currentChar() const noexcept { return currentChar_; }

    void setDefaultParameter(unsigned int value) noexcept { defaultParameter_ = value; }

    size_t parameterCount() const noexcept { return parameters_.size(); }

    unsigned int param(size_t i) const noexcept
    {
        if (i < parameters_.size() && parameters_[i])
            return parameters_[i];
        else
            return defaultParameter_;
    }

    void executeControlFunction();
    void dispatchESC();
    void dispatchCSI();
    void dispatchCSI_ext();  // "\033[? ..."
    void dispatchCSI_excl(); // "\033[! ..."
    void dispatchCSI_gt();   // "\033[> ..."
    void dispatchCSI_singleQuote(); // "\033[ ' ..."

    void setMode(unsigned int mode, bool enable);
    void setModeDEC(unsigned int mode, bool enable);
    void requestMode(unsigned int _mode);
    void requestModeDEC(unsigned int _mode);

    void dispatchGraphicsRendition();

    /// Parses color at given parameter offset @p i and returns new offset to continue processing parameters.
    template <typename T>
    size_t parseColor(size_t i);

    template <typename T, typename... Args>
    void emit(Args&&... args)
    {
        commands_.emplace_back(T{std::forward<Args>(args)...});
        // TODO: telemetry_.increment(fmt::format("{}.{}", "Command", typeid(T).name()));
    }

    template <typename Event, typename... Args>
    void log(std::string_view const& msg, Args... args) const
    {
        if (logger_)
            logger_(Event{ fmt::format(msg, args...) });
    }

    void logInvalidESC(std::string const& message = "") const;
    void logInvalidCSI(std::string const& message = "") const;
    void logUnsupportedCSI() const;
    void logUnsupported(std::string_view const& msg) const;

    template <typename... Args>
    void logUnsupported(std::string_view const& msg, Args... args) const
    {
        logUnsupported(fmt::format(msg, std::forward<Args>(args)...));
    }

    std::string sequenceString(std::string const& _prefix) const;

  private:
    char32_t currentChar_{};
    std::vector<Command> commands_{};

	char leaderSymbol_ = 0;
    std::string intermediateCharacters_{};
    std::vector<unsigned int> parameters_{0};
    unsigned int defaultParameter_ = 0;
    bool private_ = false;

    unsigned int rowCount_;

    Logger const logger_;
};

}  // namespace terminal
