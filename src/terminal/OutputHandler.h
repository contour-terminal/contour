#pragma once

#include <terminal/Parser.h>

#include <string>
#include <vector>

namespace terminal {

class OutputHandler {
  public:
    using Logger = std::function<void(std::string const&)>;
    using ActionClass = Parser::ActionClass;
    using Action = Parser::Action;

    size_t constexpr static MaxParameters = 16;

    explicit OutputHandler(size_t rows, Logger debugLogger = Logger{})
        : rowCount_{rows},
          debugLog_{std::move(debugLogger)}
    {
        parameters_.reserve(MaxParameters);
    }

    void updateRowCount(size_t rows)
    {
        rowCount_ = rows;
    }

    void invokeAction(ActionClass actionClass, Action action, wchar_t currentChar);

    void operator()(ActionClass actionClass, Action action, wchar_t currentChar)
    {
        return invokeAction(actionClass, action, currentChar);
    }

    std::vector<Command>& commands() noexcept { return commands_; }
    std::vector<Command> const& commands() const noexcept { return commands_; }

  private:
    wchar_t currentChar() const noexcept { return currentChar_; }

    void setDefaultParameter(int value) noexcept { defaultParameter_ = value; }

    size_t parameterCount() const noexcept { return parameters_.size(); }

    size_t param(size_t i) const noexcept
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
    void dispatchCSI_gt();   // "\033[> ..."

    void setMode(size_t mode, bool enable);
    void setModeDEC(size_t mode, bool enable);

    void dispatchGraphicsRendition();
    template <typename T>
    size_t parseColor(size_t i);

    template <typename T, typename... Args>
    void emit(Args&&... args)
    {
        commands_.emplace_back(T{std::forward<Args>(args)...});
        // TODO: telemetry_.increment(fmt::format("{}.{}", "Command", typeid(T).name()));
    }

    template <typename... Args>
    void log(std::string_view const& msg, Args... args) const
    {
        if (debugLog_)
            debugLog_(fmt::format(msg, args...));
    }

    void logInvalidCSI(std::string const& message = "") const;
    void logUnsupportedCSI() const;
    void logUnsupported(std::string_view const& msg) const;

    template <typename... Args>
    void logUnsupported(std::string_view const& msg, Args... args) const
    {
        logUnsupported(fmt::format(msg, std::forward<Args>(args)...));
    }

  private:
    wchar_t currentChar_{};
    std::vector<Command> commands_{};

    std::string intermediateCharacters_{};
    std::vector<size_t> parameters_{0};
    int defaultParameter_ = 0;
    bool private_ = false;

    size_t rowCount_;

    Logger const debugLog_;
};

}  // namespace terminal
