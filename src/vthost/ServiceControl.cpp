// SPDX-License-Identifier: Apache-2.0
#include <vthost/ServiceControl.hpp>

#include <core/platform/UserPaths.hpp>

#include <algorithm>
#include <format>
#include <tuple>
#include <utility>

#include <vthost/ServiceControl_launchd.hpp>
#include <vthost/ServiceControl_systemd.hpp>
#include <vthost/SocketPath.hpp>

#ifdef __APPLE__
    #include <unistd.h>
#endif

namespace vthost
{

namespace
{
    /// The unsupported-platform backend.
    ///
    /// A real object rather than a null pointer so every caller has ONE error path: a verb
    /// that must check for null before it can report a failure ends up with two ways to say
    /// the same thing, and the null branch is the one nobody writes a message for.
    class UnsupportedBackend final: public ServiceBackend
    {
      public:
        /// @param context What to tell the user: why this backend refuses, and what to do instead.
        explicit UnsupportedBackend(std::string context): _context { std::move(context) } {}

        [[nodiscard]] std::expected<void, ServiceError> install(ServiceInstallRequest const&) override
        {
            return refuse();
        }
        [[nodiscard]] std::expected<void, ServiceError> uninstall() override { return refuse(); }
        [[nodiscard]] std::expected<void, ServiceError> start() override { return refuse(); }
        [[nodiscard]] std::expected<void, ServiceError> stop() override { return refuse(); }
        [[nodiscard]] std::expected<ServiceStatus, ServiceError> status() const override
        {
            return std::unexpected(error());
        }
        [[nodiscard]] DaemonLaunchStyle launchStyle() const noexcept override
        {
            return DaemonLaunchStyle::Foreground;
        }

      private:
        [[nodiscard]] ServiceError error() const
        {
            return ServiceError { .code = ServiceErrorCode::Unsupported,
                                  .systemCode = 0,
                                  .context = _context };
        }
        [[nodiscard]] std::expected<void, ServiceError> refuse() const { return std::unexpected(error()); }

        std::string _context;
    };

    [[nodiscard]] std::string_view describe(ServiceErrorCode code) noexcept
    {
        switch (code)
        {
            case ServiceErrorCode::AlreadyInstalled: return "already installed";
            case ServiceErrorCode::NotInstalled: return "not installed";
            case ServiceErrorCode::AccessDenied: return "access denied";
            case ServiceErrorCode::NotRunning: return "not running";
            case ServiceErrorCode::Unsupported: return "unsupported on this platform";
            case ServiceErrorCode::Backend: return "the operating system refused";
            case ServiceErrorCode::NotUtf8: return "not UTF-8";
        }
        return "unknown error";
    }
} // namespace

std::string ServiceError::toString() const
{
    auto result = std::string { describe(code) };
    if (!context.empty())
        result += " (" + context + ")";
    if (systemCode != 0)
        result += std::format(" [status 0x{:08x}]", static_cast<unsigned long>(systemCode));
    return result;
}

std::string_view nameOf(ServiceStartMode mode) noexcept
{
    for (auto const& [text, value]: ServiceStartModeNames)
        if (value == mode)
            return text;
    return "logon";
}

std::optional<ServiceStartMode> serviceStartModeFrom(std::string_view text)
{
    for (auto const& [name, value]: ServiceStartModeNames)
        if (name == text)
            return value;
    return std::nullopt;
}

std::string serviceNameForLabel(std::string_view label)
{
    if (label == DefaultSocketLabel)
        return "contour-daemon";
    return std::format("contour-daemon-{}", label);
}

std::vector<std::string> registrationNamesForLabel(std::string_view label)
{
    auto names = std::vector { serviceNameForLabel(label) };
    // Before phase 2 the default label was registered as `contour-daemon-default`.
    if (auto legacy = std::format("contour-daemon-{}", label); legacy != names.front())
        names.push_back(std::move(legacy));
    return names;
}

std::expected<CommandOutcome, ServiceError> runServiceCommand(CommandRunner& commands,
                                                              std::vector<std::string> const& argv,
                                                              std::string_view context)
{
    auto outcome = commands.run(argv);
    if (!outcome)
        return std::unexpected(ServiceError { .code = ServiceErrorCode::Backend,
                                              .systemCode = outcome.error().value(),
                                              .context = std::format("{} not found", argv.front()) });
    if (outcome->exitCode != 0)
        return std::unexpected(ServiceError { .code = ServiceErrorCode::Backend,
                                              .systemCode = outcome->exitCode,
                                              .context = std::string { context } });
    return *std::move(outcome);
}

#ifndef _WIN32
std::unique_ptr<ServiceBackend> makeServiceBackend(ServiceStartMode mode,
                                                   std::string_view name,
                                                   ServiceBackendServices const& services)
{
    if (mode != ServiceStartMode::Logon)
        return std::make_unique<UnsupportedBackend>(
            "--start=boot and --start=manual are Windows-only; use --start=logon");

    #ifdef __APPLE__
    auto const home = core::platform::homeDirectory(services.environment);
    if (!home)
        return std::make_unique<UnsupportedBackend>("cannot locate the home directory (HOME is not set)");
    auto label = launchdLabelFor(name);
    auto plistPath = launchAgentPath(*home, label);
    return std::make_unique<LaunchdBackend>(LaunchdAgentConfig { .label = std::move(label),
                                                                 .plistPath = std::move(plistPath),
                                                                 .domain = launchdGuiDomain(::getuid()) },
                                            services);
    #elifdef __linux__
    auto const directory = systemdUserUnitDirectory(services.environment);
    if (directory.empty())
        return std::make_unique<UnsupportedBackend>("cannot locate the home directory (HOME is not set)");
    auto unitName = std::format("{}.service", name);
    auto unitPath = directory / unitName;
    return std::make_unique<SystemdUserBackend>(
        SystemdUnitConfig { .unitName = std::move(unitName), .unitPath = std::move(unitPath) }, services);
    #else
    std::ignore = name;
    std::ignore = services;
    return std::make_unique<UnsupportedBackend>("no service backend on this platform");
    #endif
}
#endif

} // namespace vthost
