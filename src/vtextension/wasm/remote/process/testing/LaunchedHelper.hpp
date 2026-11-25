// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Test-only: starts an executable the way Contour starts its extension host -- fresh channels, an interrupt
/// page and a wake signal, inherited and named on its command line -- and holds Contour's ends of them. What
/// the launcher's tests start the probe with, and the helper's tests the real executable.

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <vtextension/wasm/remote/process/ProcessLauncher.hpp>
#include <vtextension/wasm/remote/transport/Channel.hpp>
#include <vtextension/wasm/remote/transport/ControlPage.hpp>
#include <vtextension/wasm/remote/transport/WakeSignal.hpp>

namespace vtextension::wasm::remote::process::testing
{

/// How long a test waits for a child to do what it was told.
inline constexpr auto ChildPatience = std::chrono::seconds { 5 };

/// @return This process's id.
[[nodiscard]] ProcessId ownProcessId() noexcept;

/// @return This process's stderr, as it is now: what a launched child reports to unless a test says
/// otherwise.
[[nodiscard]] std::optional<transport::NativeHandle> ownStandardError() noexcept;

/// @return The sanitizer variables a helper is handed: this process's, so that its sanitizer runtime is
///         configured as the tests' is.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> sanitizerEnvironment();

/// Reaps @p process, waiting up to ChildPatience for it to exit.
/// @return Its status; nothing if it still ran when the patience ran out, or could not be waited for.
[[nodiscard]] std::optional<ExitStatus> awaitReap(IProcessLauncher& launcher, IProcessHandle& process);

/// Kills @p process if it still runs, and reaps it.
/// @return Whether it is reaped: whether it is gone for good.
[[nodiscard]] bool endAndReap(IProcessLauncher& launcher, IProcessHandle& process);

/// Reads a probe's report off @p channel: `key=value` lines, the last `end`.
/// @return Its lines, `end` included, once all of it arrived; nothing if the channel failed first.
[[nodiscard]] std::optional<std::vector<std::string>> readReport(transport::Channel& channel);

/// What to start, and how.
struct HelperLaunch
{
    std::filesystem::path executable;    ///< The program.
    std::vector<std::string> leading {}; ///< Arguments before the helper arguments.
    /// Arguments after them, given the handles' values in the child.
    std::function<std::vector<std::string>(std::span<transport::NativeHandle const>)> trailing {};
    ProcessId parentProcessId = ownProcessId();                      ///< What `--parent` names.
    uint64_t dataBytes = DefaultDataBytes;                           ///< What `--data-bytes` names.
    std::vector<std::pair<std::string, std::string>> environment {}; ///< The child's environment.
    /// Its stderr (LaunchSpec::standardError): by default this process's own, so that what it reports shows.
    std::optional<transport::NativeHandle> standardError = ownStandardError();
};

/// A started child: Contour's ends of everything it was handed, and the process. Its destructor ends and
/// reaps a child that a failed assertion left running.
class LaunchedHelper
{
  public:
    LaunchedHelper(std::shared_ptr<IProcessLauncher> launcher,
                   std::unique_ptr<IProcessHandle> process,
                   transport::Channel control,
                   std::vector<transport::Channel> lanes,
                   transport::ControlPage page,
                   transport::WakeSender wake,
                   std::shared_ptr<std::vector<transport::NativeHandle>> childHandles) noexcept;
    LaunchedHelper(LaunchedHelper&&) noexcept = default;
    LaunchedHelper& operator=(LaunchedHelper&&) noexcept = delete;
    LaunchedHelper(LaunchedHelper const&) = delete;
    LaunchedHelper& operator=(LaunchedHelper const&) = delete;
    ~LaunchedHelper();

    std::shared_ptr<IProcessLauncher> launcher;
    std::unique_ptr<IProcessHandle> process;
    transport::Channel control;
    std::vector<transport::Channel> lanes;
    transport::ControlPage page;
    transport::WakeSender wake;
    std::shared_ptr<std::vector<transport::NativeHandle>>
        childHandles; ///< What its arguments were made from.

    /// Ends the child the way Contour lets a helper end: its control channel reaches end of stream.
    void closeControl() noexcept;
};

/// Everything a launch of a helper is made of, before it starts: the spec, and both ends of what the spec
/// hands the child. The helper's ends are Contour's to close once the launch is done.
struct PreparedHelperLaunch
{
    LaunchSpec spec;                           ///< What the launcher is given.
    transport::ChannelPair control;            ///< The control channel.
    std::vector<transport::ChannelPair> lanes; ///< The lane channels.
    transport::ControlPage page;               ///< The interrupt page.
    transport::WakePair wake;                  ///< The wake signal.
    std::shared_ptr<std::vector<transport::NativeHandle>>
        childHandles; ///< What its arguments were made from.
};

/// Makes fresh channels, page and wake for @p launch, and the spec that hands them over.
/// @return The spec and the ends.
[[nodiscard]] PreparedHelperLaunch prepareHelperLaunch(HelperLaunch launch);

/// Starts @p launch with fresh channels, page and wake.
/// @return The child; why it could not be started otherwise.
[[nodiscard]] std::expected<LaunchedHelper, LaunchError> launchHelper(
    std::shared_ptr<IProcessLauncher> launcher, HelperLaunch launch);

} // namespace vtextension::wasm::remote::process::testing
