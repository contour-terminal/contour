// SPDX-License-Identifier: Apache-2.0
#include <vthost/ServiceControl.h>

#include <algorithm>
#include <format>
#include <utility>

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

      private:
        [[nodiscard]] static ServiceError error()
        {
            return ServiceError { .code = ServiceErrorCode::Unsupported,
                                  .systemCode = 0,
                                  .context = "no service backend on this platform" };
        }
        [[nodiscard]] static std::expected<void, ServiceError> refuse() { return std::unexpected(error()); }
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
    return std::format("contour-daemon-{}", label);
}

#ifndef _WIN32
std::unique_ptr<ServiceBackend> makeServiceBackend(ServiceStartMode /*mode*/, std::string_view /*name*/)
{
    // POSIX equivalents exist (a systemd USER unit is the close analogue of the logon task,
    // and launchd's LaunchAgents the macOS one), but neither is implemented yet. The seam is
    // here; what is missing is a backend, not a design.
    return std::make_unique<UnsupportedBackend>();
}
#endif

} // namespace vthost
