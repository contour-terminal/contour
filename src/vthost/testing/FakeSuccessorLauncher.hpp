// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A successor launcher that starts no process, for the tests of the Windows handoff: its "successor" is an
/// event that stands in for the process handle, signalled once the successor has ended.

#ifdef _WIN32

    #include <vtpty/WindowsHandle.hpp>

    #include <cstdint>
    #include <exception>
    #include <expected>
    #include <filesystem>
    #include <functional>
    #include <optional>
    #include <span>
    #include <string>
    #include <string_view>
    #include <system_error>
    #include <vector>

    #include <Windows.h>

    #include <vthost/handover/ImageHandoff.hpp>
    #include <vthost/handover/SuccessorLaunch.hpp>

namespace vthost::testing
{

/// What a FakeSuccessorLauncher's successor does once it is launched.
enum class SuccessorBehaviour : uint8_t
{
    TakesOver = 0,   ///< Runs onLaunch, then says it is ready.
    EndsBeforeReady, ///< Ends at once, having said nothing.
    NeverReady,      ///< Runs, and never says anything.
    FailsToStart,    ///< Cannot be started at all.
};

/// Thrown from endThisProcess(): it ends the handoff as the old daemon's end would.
struct SuccessorTookOver: std::exception
{
};

/// A SuccessorLauncher that records what it was asked, and plays the successor as told.
class FakeSuccessorLauncher final: public handover::SuccessorLauncher
{
  public:
    /// A launch asked for.
    struct Launch
    {
        std::filesystem::path executable;          ///< What was to run.
        std::vector<std::string> argv;             ///< Its command line.
        std::vector<vtpty::HandleValue> inherited; ///< What it was to inherit.
        handover::SuccessorHandles handles;        ///< What its `--handover-handles` named.
    };

    explicit FakeSuccessorLauncher(SuccessorBehaviour behaviour = SuccessorBehaviour::TakesOver):
        _behaviour { behaviour }
    {
    }
    FakeSuccessorLauncher(FakeSuccessorLauncher const&) = delete;
    FakeSuccessorLauncher& operator=(FakeSuccessorLauncher const&) = delete;
    FakeSuccessorLauncher(FakeSuccessorLauncher&&) = delete;
    FakeSuccessorLauncher& operator=(FakeSuccessorLauncher&&) = delete;
    ~FakeSuccessorLauncher() override
    {
        if (_process != nullptr)
            CloseHandle(_process);
    }

    [[nodiscard]] std::expected<handover::SuccessorProcess, std::error_code> launch(
        std::filesystem::path const& executable,
        std::span<std::string const> argv,
        std::span<vtpty::HandleValue const> inherited) override
    {
        auto const argument = argv.empty() ? std::string_view {} : std::string_view { argv.back() };
        auto const prefix = handover::HandoverHandlesPrefix;
        auto const handles = argument.starts_with(prefix)
                                 ? handover::parseHandoverHandles(argument.substr(prefix.size()))
                                 : std::nullopt;
        launches.push_back(Launch { .executable = executable,
                                    .argv = { argv.begin(), argv.end() },
                                    .inherited = { inherited.begin(), inherited.end() },
                                    .handles = handles.value_or(handover::SuccessorHandles {}) });
        if (_behaviour == SuccessorBehaviour::FailsToStart)
            return std::unexpected(std::make_error_code(std::errc::no_such_file_or_directory));

        // What the old daemon may still do while its successor starts: whatever it is, the successor's own
        // behaviour follows.
        if (onLaunch && handles)
            onLaunch(*handles);
        _process = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        auto handed = HANDLE {};
        DuplicateHandle(
            GetCurrentProcess(), _process, GetCurrentProcess(), &handed, 0, FALSE, DUPLICATE_SAME_ACCESS);
        switch (_behaviour)
        {
            case SuccessorBehaviour::TakesOver:
                if (handles)
                    SetEvent(vtpty::handleOf(handles->ready));
                break;
            case SuccessorBehaviour::EndsBeforeReady: SetEvent(_process); break;
            case SuccessorBehaviour::NeverReady:
            case SuccessorBehaviour::FailsToStart: break;
        }
        return handover::SuccessorProcess { .process = vtpty::valueOf(handed), .pid = SuccessorPid };
    }

    void terminate(handover::SuccessorProcess const& /*successor*/) override
    {
        ++terminated;
        SetEvent(_process);
    }

    [[noreturn]] void endThisProcess() override
    {
        auto const& handles = launches.back().handles;
        committed = WaitForSingleObject(vtpty::handleOf(handles.commit), 0) == WAIT_OBJECT_0;
        throw SuccessorTookOver {};
    }

    /// The process id every successor is given.
    static constexpr auto SuccessorPid = std::int64_t { 4242 };

    /// Runs as a successor starts, before it does what its behaviour says: what reads the image.
    std::function<void(handover::SuccessorHandles const&)> onLaunch;

    std::vector<Launch> launches; ///< Every launch asked for, in order.
    int terminated = 0;           ///< How often a successor was ended.
    bool committed = false;       ///< Whether the commit was set as this process was ended.

  private:
    SuccessorBehaviour _behaviour;
    HANDLE _process = nullptr; ///< What stands in for the successor process: signalled once it has ended.
};

} // namespace vthost::testing

#endif
