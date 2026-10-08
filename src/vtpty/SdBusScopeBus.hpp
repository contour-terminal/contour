// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/ScopeBus.hpp>
#include <vtpty/SystemdScopePlacement.hpp>

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

    /// Connects to the user bus, and asks systemd for job signals.
    /// @param timeout How long each call on the connection may take, this first one included.
    /// @return The bus, or Unavailable when there is no user bus or no systemd instance on it.
    [[nodiscard]] static std::expected<std::unique_ptr<ScopeBus>, ScopeError> connect(
        std::chrono::milliseconds timeout);

    /// Takes over an open connection. Use connect(), which also subscribes it.
    /// @param bus The connection.
    explicit SdBusScopeBus(BusHandle bus) noexcept;

    [[nodiscard]] std::expected<void, ScopeError> startScope(ScopeRequest const& request,
                                                             std::chrono::milliseconds deadline) override;

    /// Notes that systemd removed the job of @p unit with @p result. Called by the JobRemoved handler.
    /// @param unit   The unit the job was for.
    /// @param result How the job ended: `done`, `failed`, `canceled`, ...
    void jobRemoved(std::string_view unit, std::string_view result);

  private:
    [[nodiscard]] std::expected<void, ScopeError> awaitJob(std::chrono::steady_clock::time_point until);

    BusHandle _bus;
    std::string _awaitedUnit;                  ///< The unit whose job startScope() waits for.
    std::optional<std::string> _awaitedResult; ///< Its job's result, once JobRemoved told it.
};

/// @param config How to place children.
/// @return A SystemdScopePlacement talking to the user's systemd through sd-bus, on the steady clock.
[[nodiscard]] std::shared_ptr<ProcessPlacement> makeSystemdScopePlacement(SystemdScopeConfig config);

} // namespace vtpty
