// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The connector that starts each extension host as a process of its own, through the supervisor: the
/// production IHostConnector.

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

/// Starts each host as a process the supervisor launches and owns, connected over inherited channels, an
/// interrupt page and a wake signal.
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
    ///         -- logged the first time for each reason the system gave.
    [[nodiscard]] std::expected<remote::HostConnection, ExtensionError> connect() override;

  private:
    std::shared_ptr<SupervisorMailbox> _supervisor;
    ProcessHostConfig _config;
    remote::HostRefusalLog _refusals;
};

} // namespace vtextension::wasm::remote::process
