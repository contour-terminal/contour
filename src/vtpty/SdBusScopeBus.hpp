// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/ScopeBus.hpp>
#include <vtpty/SystemdScopePlacement.hpp>

#include <core/platform/Clock.hpp>

#include <chrono>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

struct sd_bus;

namespace vtpty
{

/// A ScopeBus over sd-bus, talking to the calling user's systemd instance.
///
/// Not thread-safe, as sd-bus itself is not: SystemdScopePlacement calls it from its worker only.
class SdBusScopeBus final: public ScopeBus
{
  public:
    /// Releases an sd_bus connection, flushing it first.
    struct BusDeleter
    {
        void operator()(sd_bus* bus) const noexcept;
    };
    using BusHandle = std::unique_ptr<sd_bus, BusDeleter>;

    /// Connects to the user bus.
    /// @param clock The clock request deadlines are measured on; must outlive the bus.
    /// @return The bus, or Unavailable when there is no user bus.
    [[nodiscard]] static std::expected<std::unique_ptr<ScopeBus>, ScopeError> connect(
        core::platform::IClock const& clock);

    /// Takes over an open connection. Use connect().
    /// @param bus   The connection.
    /// @param clock The clock request deadlines are measured on; must outlive this.
    SdBusScopeBus(BusHandle bus, core::platform::IClock const& clock) noexcept;

    [[nodiscard]] std::expected<void, ScopeError> startScope(ScopeRequest const& request,
                                                             std::chrono::milliseconds deadline) override;

    /// Notes that systemd removed the job of @p unit with @p result. Called by the JobRemoved handler.
    /// @param unit   The unit the job was for.
    /// @param result How the job ended: `done`, `failed`, `canceled`, ...
    void jobRemoved(std::string_view unit, std::string_view result);

  private:
    [[nodiscard]] std::expected<void, ScopeError> awaitJob(core::platform::SteadyTimePoint until);
    void cancelJob(std::string const& jobPath) noexcept;

    BusHandle _bus;
    core::platform::IClock const& _clock;
    std::string _awaitedUnit;                  ///< The unit whose job startScope() waits for.
    std::optional<std::string> _awaitedResult; ///< Its job's result, once JobRemoved told it.
};

/// @param config How to place children.
/// @return A SystemdScopePlacement talking to the user's systemd through sd-bus, on the steady clock.
[[nodiscard]] std::shared_ptr<ProcessPlacement> makeSystemdScopePlacement(SystemdScopeConfig config);

} // namespace vtpty
