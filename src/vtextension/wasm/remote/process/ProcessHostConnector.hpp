// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The connector that starts each extension host as a process of its own, through the supervisor: the
/// production IHostConnector.

#include <atomic>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <vtextension/ExtensionError.hpp>
#include <vtextension/wasm/remote/HostConnector.hpp>
#include <vtextension/wasm/remote/HostRefusals.hpp>
#include <vtextension/wasm/remote/process/ExtensionHostSupervisor.hpp>
#include <vtextension/wasm/remote/process/ProcessLauncher.hpp>

namespace vtextension::wasm::remote::process
{

/// How each host process is started.
struct ProcessHostConfig
{
    std::filesystem::path helper; ///< The extension host executable; an absolute path.
    /// Its whole environment: by default what any host needs (defaultHelperEnvironment()).
    std::vector<std::pair<std::string, std::string>> environment = defaultHelperEnvironment();
    std::vector<std::string> extraArguments; ///< Appended after the handle arguments; for test helpers.
    ResourceCaps caps {};                    ///< The caps it runs under.
    /// Where its stderr goes (LaunchSpec::standardError): by default the null device, or this process's own
    /// in a build whose helpers report diagnostics (defaultHelperStandardError()).
    std::optional<transport::NativeHandle> standardError = defaultHelperStandardError();
};

/// Whether a host this connector started ever failed to confine itself.
enum class LockdownHistory : uint8_t
{
    NeverFailed, ///< None did, or none was started.
    Failed,      ///< One did: a property of the system and the build, which every later host would share.
};

/// Starts each host as a process the supervisor launches and owns, connected over inherited channels, an
/// interrupt page and a wake signal. Once a host it started was refused for failing to confine itself, it
/// starts none again: a host would fail the same way, on this system and build, for every extension.
class ProcessHostConnector final: public remote::IHostConnector
{
  public:
    /// @param supervisor What launches and supervises the processes.
    /// @param config How each one is started.
    /// @param refusals Where why a host could not be started is logged, once per reason: errorLogWriter() in
    ///        Contour.
    ProcessHostConnector(std::shared_ptr<SupervisorMailbox> supervisor,
                         ProcessHostConfig config,
                         remote::HostRefusalLog::Writer refusals);

    /// Creates the channels, the page and the wake signal, and has the supervisor start a host on their
    /// helper ends, which close here once it started.
    /// @return The connection; `HostUnavailable` if any of it could not be created, or the host not started
    ///         -- logged the first time for each reason the system gave -- and, without starting one, once a
    ///         host it started failed to confine itself (refused()).
    [[nodiscard]] std::expected<remote::HostConnection, ExtensionError> connect() override;

    /// Keeps a `LockdownFailed` refusal for good: every later connect() fails without starting a host,
    /// until the connector is made again. Thread-safe.
    /// @param refusal Why the host was refused.
    void refused(remote::HostRefusal refusal) override;

  private:
    std::shared_ptr<SupervisorMailbox> _supervisor;
    ProcessHostConfig _config;
    remote::HostRefusalLog _refusals;
    std::atomic<LockdownHistory> _lockdown { LockdownHistory::NeverFailed };
};

} // namespace vtextension::wasm::remote::process
