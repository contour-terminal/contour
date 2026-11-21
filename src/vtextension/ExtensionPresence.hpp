// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string_view>

namespace vtextension
{

/// Where a daemon-scoped extension stands.
enum class DaemonExtensionPhase : uint8_t
{
    Stopped = 0, ///< Not running, and not waiting for its user.
    Pending,     ///< It was to start, and waits for its user's grant.
    Denied,      ///< Its user refused it.
    Running,     ///< Its runner runs it, or recovers it from a fault.
    Disabled,    ///< It faulted too often; it runs no more until started again.
    Failed,      ///< It was granted and started, and could not run.
};

/// Which run of an extension something comes from. Revoking an extension's grant starts a new one, so that
/// what the run before it still publishes, from its lane, is told apart and dropped.
using DaemonRunGeneration = uint64_t;

/// Where a daemon-world extension shows itself: its summary (`summary-set`) and where it stands, for the
/// status line's segment and `contour extension status`. The text it receives is bounded and neutralized
/// already.
///
/// Every call carries the run it comes from. One older than the latest run told is to be dropped; a newer one
/// replaces what the older run showed.
class IExtensionPresence
{
  public:
    IExtensionPresence() = default;
    IExtensionPresence(IExtensionPresence const&) = delete;
    IExtensionPresence(IExtensionPresence&&) = delete;
    IExtensionPresence& operator=(IExtensionPresence const&) = delete;
    IExtensionPresence& operator=(IExtensionPresence&&) = delete;
    virtual ~IExtensionPresence() = default;

    /// Replaces what an extension shows of itself. Called from any thread; must be cheap and must not throw.
    /// @param extension What the configuration calls the extension.
    /// @param generation The run that published it.
    /// @param shortText The one-line summary.
    /// @param detail The longer text; its lines are separated by `\n`.
    virtual void publishSummary(std::string_view extension,
                                DaemonRunGeneration generation,
                                std::string_view shortText,
                                std::string_view detail) = 0;

    /// Tells where an extension now stands. Called from any thread; must be cheap and must not throw.
    /// @param extension What the configuration calls the extension.
    /// @param generation The run it concerns.
    /// @param phase Where it stands.
    virtual void phaseChanged(std::string_view extension,
                              DaemonRunGeneration generation,
                              DaemonExtensionPhase phase) = 0;
};

} // namespace vtextension
