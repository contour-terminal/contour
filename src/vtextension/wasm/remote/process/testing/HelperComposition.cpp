// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/remote/process/testing/HelperComposition.hpp>

#include <core/Clock.hpp>
#include <core/Environment.hpp>
#include <core/log/LogStore.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <vtextension/wasm/remote/transport/Channel.hpp>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <Windows.h>
#else
    #include <fcntl.h>
    #include <unistd.h>
#endif

namespace vtextension::wasm::remote::process::testing
{

namespace
{
    /// The sanitizer variables a helper is handed when this process has them, so that its sanitizer runtime
    /// is configured as the tests' is.
    constexpr auto SanitizerVariables =
        std::array<std::string_view, 4> { "ASAN_OPTIONS", "LSAN_OPTIONS", "UBSAN_OPTIONS", "TSAN_OPTIONS" };

    /// How long past the teardown grace awaitLiveHelpersAtMost() waits: a killed helper's reap included.
    constexpr auto ReapSlack = std::chrono::seconds { 3 };

    /// @return A copy of this process's stderr, never closed; nothing if it has none.
    [[nodiscard]] std::optional<transport::NativeHandle> copyOfStandardError() noexcept
    {
#ifdef _WIN32
        auto* const own = ::GetStdHandle(STD_ERROR_HANDLE);
        auto copy = HANDLE { nullptr };
        if (own == nullptr || own == INVALID_HANDLE_VALUE
            || !::DuplicateHandle(
                ::GetCurrentProcess(), own, ::GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS))
            return std::nullopt;
        return copy;
#else
        auto const copy = ::fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 3);
        return copy == -1 ? std::nullopt : std::optional<transport::NativeHandle> { copy };
#endif
    }

    /// This process's stderr as it was when the binary started -- before any case captured it -- which every
    /// helper inherits as its stderr, so that what a helper reports reaches the test's log and no case's
    /// capture.
    auto const originalStandardError = copyOfStandardError();

    /// Writes @p text to originalStandardError, if there is one.
    void writeToOriginalStandardError(std::string_view text) noexcept
    {
        if (!originalStandardError)
            return;
#ifdef _WIN32
        auto written = DWORD { 0 };
        while (!text.empty()
               && ::WriteFile(
                   *originalStandardError, text.data(), static_cast<DWORD>(text.size()), &written, nullptr)
               && written != 0)
            text.remove_prefix(written);
#else
        while (!text.empty())
        {
            auto const written = ::write(*originalStandardError, text.data(), text.size());
            if (written <= 0)
                return;
            text.remove_prefix(static_cast<std::size_t>(written));
        }
#endif
    }

    /// The error log, on originalStandardError: what the supervisor says of a helper that ended uncleanly --
    /// its exit code, or the signal it died of, which a helper the system call filter killed cannot report
    /// itself -- reaches the test's log beside the helper's own report. A program's console sink stays off
    /// until it is configured, and a case may capture this process's stderr meanwhile.
    ///
    /// Installed once, before the first supervisor starts: a running supervisor reads the sink, so a
    /// later composition must not write it again.
    void logErrorsToOriginalStandardError()
    {
        static auto sink = core::log::Sink { true, [](std::string_view const& text) {
                                                writeToOriginalStandardError("[error] ");
                                                writeToOriginalStandardError(text);
                                            } };
        static std::once_flag installed;
        std::call_once(installed, [] { core::log::errorLog.setSink(sink); });
    }
} // namespace

std::vector<std::pair<std::string, std::string>> helperEnvironment()
{
    auto environment = defaultHelperEnvironment();
    auto const live = core::LiveEnvironment {};
    for (auto const name: SanitizerVariables)
        if (auto value = live.get(name))
            environment.emplace_back(std::string { name }, *std::move(value));
    return environment;
}

HelperComposition::HelperComposition(std::chrono::milliseconds teardownGrace, ArgumentRewrite laterArguments):
    _teardownGrace { teardownGrace },
    _recording { std::make_shared<RecordingLauncher>(makeSystemProcessLauncher(),
                                                     std::move(laterArguments)) },
    _supervisor { [this] {
        logErrorsToOriginalStandardError();
        auto started = ExtensionHostSupervisor::start(SupervisorConfig {
            .launcher = _recording, .clock = &core::defaultSteadyClock(), .teardownGrace = _teardownGrace });
        if (!started)
        {
            std::fputs("HelperComposition: the supervisor could not start\n", stderr);
            std::abort();
        }
        return std::move(*started);
    }() },
    _kind { makeRemoteKindState() }
{
}

std::shared_ptr<ProcessHostConnector> HelperComposition::connector(HelperBackendConfig const& config) const
{
    return std::make_shared<ProcessHostConnector>(
        _supervisor->mailbox(),
        ProcessHostConfig { .helper = config.helper,
                            .environment = helperEnvironment(),
                            .extraArguments = config.extraArguments,
                            .caps = {},
                            .standardError = originalStandardError });
}

std::unique_ptr<IWasmBackend> HelperComposition::backend(HelperBackendConfig const& config) const
{
    return makeRemoteBackend(_kind,
                             BackendConfig { .helper = config.helper,
                                             .timeouts = config.timeouts,
                                             .connector = connector(config),
                                             .lockdown = config.lockdown });
}

std::size_t HelperComposition::killsFor(KillReason reason) const
{
    // A kill's onDeath runs before its batch's counters are published: sync() first.
    _supervisor->sync();
    return _supervisor->counters().kills.at(std::to_underlying(reason));
}

bool HelperComposition::awaitEveryHelperReaped() const
{
    return awaitLiveHelpersAtMost(0);
}

bool HelperComposition::awaitLiveHelpersAtMost(std::size_t count) const
{
    using namespace std::chrono_literals;
    auto const deadline = std::chrono::steady_clock::now() + _teardownGrace + ReapSlack;
    while (_supervisor->liveProcesses() > count && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    return _supervisor->liveProcesses() <= count;
}

HelperComposition& binaryComposition()
{
    static auto composition = HelperComposition {};
    return composition;
}

} // namespace vtextension::wasm::remote::process::testing
