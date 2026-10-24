// SPDX-License-Identifier: Apache-2.0
//
// Test-only: an IHostServices that records what a guest asks of it and answers from fields a test
// sets, and a guest running on it.
#pragma once

#include <core/platform/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vtextension/CompiledExtension.hpp>
#include <vtextension/Engine.hpp>
#include <vtextension/IHostServices.hpp>
#include <vtextension/Instance.hpp>
#include <vtextension/wasm/DefaultBackend.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>

namespace vtextension::testing
{

/// @return The error @p result holds; `std::nullopt` if it holds a value. Unlike
///         `result.error()`, safe to call on a result that unexpectedly succeeded.
template <typename T>
[[nodiscard]] std::optional<ExtensionError> errorOf(std::expected<T, ExtensionError> const& result)
{
    if (result)
        return std::nullopt;
    return result.error();
}

/// One message a guest logged.
struct LoggedMessage
{
    LogLevel level;
    std::string message;

    [[nodiscard]] bool operator==(LoggedMessage const&) const = default;
};

/// One command a guest asked to run.
struct CommandCall
{
    std::string name;
    std::vector<std::pair<std::string, std::string>> args;

    [[nodiscard]] bool operator==(CommandCall const&) const = default;
};

/// One control verb a guest called.
struct ControlCall
{
    std::string method;
    std::string params;
    vtcontrol::ControlGrants granted;
    std::chrono::milliseconds timeout;

    [[nodiscard]] bool operator==(ControlCall const&) const = default;
};

/// Records every host call; answers `state` with `snapshot`, `run-command` with `commandAnswer`, and the
/// control verbs with `consent` and `controlAnswer`.
class FakeHostServices final: public IHostServices
{
  public:
    void requestFrame(std::chrono::milliseconds after) override { frameRequests.push_back(after); }

    void log(LogLevel level, std::string_view message) override
    {
        logged.push_back(LoggedMessage { .level = level, .message = std::string(message) });
    }

    [[nodiscard]] StateSnapshot state() const override { return snapshot; }

    [[nodiscard]] std::expected<std::string, ExtensionError> runCommand(
        std::string_view name, std::span<std::pair<std::string, std::string> const> args) override
    {
        commands.push_back(CommandCall { .name = std::string(name), .args = { args.begin(), args.end() } });
        if (beforeCommandAnswer)
            beforeCommandAnswer();
        return commandAnswer;
    }

    [[nodiscard]] GrantState controlConsent() override
    {
        ++consentQuestions;
        return consent;
    }

    [[nodiscard]] std::expected<std::string, vtcontrol::ControlError> callControl(
        std::string_view method,
        std::string_view paramsJson,
        vtcontrol::ControlGrants granted,
        std::chrono::milliseconds timeout) override
    {
        controlCalls.push_back(ControlCall { .method = std::string(method),
                                             .params = std::string(paramsJson),
                                             .granted = granted,
                                             .timeout = timeout });
        if (beforeControlAnswer)
            beforeControlAnswer();
        return controlAnswer;
    }

    std::vector<std::chrono::milliseconds> frameRequests;
    std::vector<LoggedMessage> logged;
    std::vector<CommandCall> commands;
    StateSnapshot snapshot;
    std::expected<std::string, ExtensionError> commandAnswer;
    std::function<void()> beforeCommandAnswer; ///< Runs inside runCommand, before it answers.

    GrantState consent = GrantState::Granted; ///< What controlConsent() answers.
    int consentQuestions = 0;                 ///< How often controlConsent() was asked.
    std::vector<ControlCall> controlCalls;    ///< Every callControl(), in order.
    std::expected<std::string, vtcontrol::ControlError> controlAnswer; ///< What callControl() answers.
    std::function<void()> beforeControlAnswer; ///< Runs inside callControl, before it answers.
};

/// @return A surface of @p columns by @p lines cells.
[[nodiscard]] inline vtpty::PageSize surface(int columns, int lines)
{
    return vtpty::PageSize { .lines = vtpty::LineCount(lines), .columns = vtpty::ColumnCount(columns) };
}

/// A guest module loaded on its own engine, of its own default backend, with one instance of it running
/// on a FakeHostServices.
class RunningGuest
{
  public:
    /// Loads @p module and instantiates it under @p limits.
    explicit RunningGuest(std::vector<std::byte> module, ExtensionLimits limits = {}):
        _backend { wasm::makeDefaultBackend() }, _limits { limits }
    {
        auto engine = Engine::create(*_backend, limits);
        REQUIRE(engine.has_value());
        _engine = std::move(*engine);
        auto compiled = CompiledExtension::load(*_engine, std::move(module));
        REQUIRE(compiled.has_value());
        _compiled = std::move(*compiled);
        restart();
    }

    /// Replaces the instance with a fresh one of the same module.
    void restart()
    {
        _instance.reset();
        auto instance = Instance::create(*_engine, *_compiled, services, clock, _limits);
        REQUIRE(instance.has_value());
        _instance = std::move(*instance);
    }

    /// @return The backend the engine was created from.
    [[nodiscard]] wasm::IWasmBackend& backend() const { return *_backend; }

    /// @return The running instance.
    [[nodiscard]] Instance& instance() const { return *_instance; }

    /// @return The loaded module.
    [[nodiscard]] CompiledExtension const& compiled() const { return *_compiled; }

    /// @return The engine.
    [[nodiscard]] Engine& engine() const { return *_engine; }

    /// Loads the guest with @p config, which must succeed, and renders it at 80x1.
    [[nodiscard]] std::string loadAndRender(ConfigMap const& config) const
    {
        REQUIRE(instance().load(config).has_value());
        auto rendered = instance().render(surface(80, 1));
        REQUIRE(rendered.has_value());
        return std::move(*rendered);
    }

    FakeHostServices services;         ///< Declared first: outlives the instance that calls it.
    core::platform::ManualClock clock; ///< What the instance measures its deadlines' use by.

  private:
    std::unique_ptr<wasm::IWasmBackend> _backend; ///< Outlives the engine created from it.
    ExtensionLimits _limits;
    std::shared_ptr<Engine> _engine;
    std::shared_ptr<CompiledExtension> _compiled;
    std::unique_ptr<Instance> _instance;
};

} // namespace vtextension::testing
