// SPDX-License-Identifier: Apache-2.0
#include <vthost/CommandRunner.hpp>

#ifndef _WIN32

    #include <vtpty/ProcessRunner.hpp>
    #include <vtpty/UnixUtils.hpp>

    #include <core/platform/Clock.hpp>

    #include <chrono>
    #include <cstddef>
    #include <expected>
    #include <future>
    #include <limits>
    #include <memory>
    #include <optional>
    #include <span>
    #include <string>
    #include <string_view>
    #include <system_error>
    #include <utility>
    #include <vector>

namespace vthost
{

namespace
{
    /// How long run() lets a command take: long enough to mean "until it ends", bounded all the same.
    constexpr auto RunTimeout = std::chrono::milliseconds { std::chrono::hours { 24 } };

    /// @return @p entries ("NAME=VALUE" each) as name and value; an entry without '=' is a name with an
    ///         empty value.
    template <typename Entries>
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> namesAndValues(Entries const& entries)
    {
        auto result = std::vector<std::pair<std::string, std::string>> {};
        result.reserve(entries.size());
        for (std::string_view const entry: entries)
        {
            auto const separator = entry.find('=');
            if (separator == std::string_view::npos)
                result.emplace_back(entry, std::string {});
            else
                result.emplace_back(entry.substr(0, separator), entry.substr(separator + 1));
        }
        return result;
    }

    /// The reasons a process runner could not be made, as error codes: each names its reason, and compares
    /// equal to `std::errc::resource_unavailable_try_again`.
    class ProcessRunnerErrorCategory final: public std::error_category
    {
      public:
        [[nodiscard]] char const* name() const noexcept override { return "vtpty.process-runner"; }

        [[nodiscard]] std::string message(int value) const override
        {
            switch (static_cast<vtpty::ProcessRunnerError>(value))
            {
                case vtpty::ProcessRunnerError::WakeChannelUnavailable:
                    return "the process runner's wake pipe could not be made";
                case vtpty::ProcessRunnerError::ThreadUnavailable:
                    return "the process runner's thread could not be started";
            }
            return "the process runner could not be made";
        }

        [[nodiscard]] std::error_condition default_error_condition(int /*value*/) const noexcept override
        {
            return std::errc::resource_unavailable_try_again;
        }
    };

    /// @return @p error as an error code that carries its reason.
    [[nodiscard]] std::error_code runnerError(vtpty::ProcessRunnerError error) noexcept
    {
        static auto const category = ProcessRunnerErrorCategory {};
        return std::error_code { static_cast<int>(error), category };
    }

    /// @return What a process that could not be started is reported as.
    [[nodiscard]] std::error_code startError(vtpty::ProcessStartError error) noexcept
    {
        switch (error)
        {
            case vtpty::ProcessStartError::DirectoryMissing:
            case vtpty::ProcessStartError::ProgramNotFound:
                return std::make_error_code(std::errc::no_such_file_or_directory);
            case vtpty::ProcessStartError::SpawnFailed:
                return std::make_error_code(std::errc::resource_unavailable_try_again);
            case vtpty::ProcessStartError::Unsupported: return std::make_error_code(std::errc::not_supported);
        }
        return std::make_error_code(std::errc::not_supported);
    }

    /// @return What a process that ended is reported as: its exit code, 128 + the signal that ended it, or
    ///         `timed_out`.
    [[nodiscard]] std::expected<CommandOutcome, std::error_code> outcomeOf(vtpty::ProcessExit exit)
    {
        if (exit.completion == vtpty::ProcessCompletion::TimedOut)
            return std::unexpected(std::make_error_code(std::errc::timed_out));
        if (exit.completion == vtpty::ProcessCompletion::Cancelled)
            return std::unexpected(std::make_error_code(std::errc::operation_canceled));
        if (exit.completion == vtpty::ProcessCompletion::StatusLost)
            return std::unexpected(std::make_error_code(std::errc::no_child_process));
        return CommandOutcome {
            .exitCode = exit.signal ? 128 + *exit.signal : exit.exitCode.value_or(0),
            .standardOutput = std::move(exit.standardOutput.bytes),
        };
    }

    class NativeCommandRunner final: public CommandRunner
    {
      public:
        /// @param environment What every command runs under ("NAME=VALUE"), its program looked up on that
        ///        environment's PATH; std::nullopt for this process's own, as it stands at each run.
        explicit NativeCommandRunner(std::optional<std::vector<std::string>> environment):
            _environment { std::move(environment) },
            _processes { vtpty::ThreadedProcessRunner::create(core::platform::defaultSteadyClock(),
                                                              vtpty::ProcessRunnerTuning {}) }
        {
        }

        [[nodiscard]] std::expected<CommandOutcome, std::error_code> run(
            std::span<std::string const> argv) override
        {
            return execute(argv, RunTimeout);
        }

        [[nodiscard]] std::expected<CommandOutcome, std::error_code> runWithin(
            std::span<std::string const> argv, std::chrono::milliseconds deadline) override
        {
            return execute(argv, deadline);
        }

      private:
        [[nodiscard]] std::expected<CommandOutcome, std::error_code> execute(
            std::span<std::string const> argv, std::chrono::milliseconds timeout)
        {
            if (argv.empty())
                return std::unexpected(std::make_error_code(std::errc::invalid_argument));
            if (!_processes)
                return std::unexpected(runnerError(_processes.error()));

            // Shared with the handler, which sets it on the runner's thread and may still be inside
            // set_value() when the wait below returns.
            auto exited = std::make_shared<std::promise<vtpty::ProcessExit>>();
            auto ended = exited->get_future();
            auto const started =
                (*_processes)
                    ->start(
                        vtpty::ProcessSpec {
                            .argv = { argv.begin(), argv.end() },
                            .workingDirectory = std::nullopt,
                            .environment = _environment ? namesAndValues(*_environment)
                                                        : namesAndValues(vtpty::util::environmentEntries()),
                            .timeout = timeout,
                            // All of standard output, as CommandOutcome promises; standard error is drained
                            // and dropped.
                            .outputCap = std::numeric_limits<std::size_t>::max(),
                            .standardOutput = vtpty::StreamCapture::Capture,
                            .standardError = vtpty::StreamCapture::Discard,
                        },
                        [exited](vtpty::ProcessToken, vtpty::ProcessExit exit) {
                            exited->set_value(std::move(exit));
                        });
            if (!started)
                return std::unexpected(startError(started.error()));
            return outcomeOf(ended.get());
        }

        std::optional<std::vector<std::string>> _environment;
        /// The runner every command goes through; or why it could not be made, which every run answers.
        std::expected<std::unique_ptr<vtpty::ThreadedProcessRunner>, vtpty::ProcessRunnerError> _processes;
    };
} // namespace

std::unique_ptr<CommandRunner> makeNativeCommandRunner()
{
    return std::make_unique<NativeCommandRunner>(std::nullopt);
}

std::unique_ptr<CommandRunner> makeNativeCommandRunner(std::vector<std::string> environment)
{
    return std::make_unique<NativeCommandRunner>(std::move(environment));
}

} // namespace vthost

#endif // !_WIN32
