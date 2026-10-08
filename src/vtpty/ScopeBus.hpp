// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace vtpty
{

/// Which identifier a scope request names its process by.
enum class ProcessReference : std::uint8_t
{
    Pid,   ///< `PIDs=[pid]`, understood by every systemd.
    PidFd, ///< `PIDFDs=[pidfd]`, immune to pid reuse; systemd 253 and later.
};

/// A memory ceiling for a scope. Tests use it to provoke an out-of-memory kill inside the scope.
struct MemoryLimit
{
    std::uint64_t maxBytes;     ///< `MemoryMax=`
    std::uint64_t swapMaxBytes; ///< `MemorySwapMax=`
};

/// One request to systemd for a session scope.
struct ScopeRequest
{
    std::string unitName;                   ///< `<name>.scope`, unique while the scope exists.
    std::string slice;                      ///< The slice the scope goes into.
    std::string description;                ///< What `systemctl --user status` shows for it.
    ProcessReference reference;             ///< Which of the next two names the process.
    int pid;                                ///< The process to move.
    int pidfd;                              ///< A pidfd for it; meaningful only for ProcessReference::PidFd.
    std::optional<MemoryLimit> memoryLimit; ///< A ceiling for the scope, if any.
};

/// Why a scope was not created.
enum class ScopeError : std::uint8_t
{
    Unavailable,     ///< No user bus, or no systemd user instance on it.
    Disconnected,    ///< The bus connection broke.
    TimedOut,        ///< No answer before the deadline.
    Refused,         ///< systemd answered with an error, or the job did not end as `done`.
    UnknownProperty, ///< systemd does not know a property of the request (PIDFDs before 253).
};

/// What an error says about the bus itself.
enum class BusHealth : std::uint8_t
{
    Responsive,   ///< systemd answered; the bus works.
    Unresponsive, ///< Nobody answered; asking again soon would cost the same deadline.
};

/// Whether an error is worth telling the user about.
enum class ScopeErrorSeverity : std::uint8_t
{
    Expected, ///< A valid configuration, such as a system without systemd.
    Fault,    ///< Something that should have worked did not.
};

/// Everything known about one ScopeError: one row per enumerator, in enumerator order.
struct ScopeErrorTraits
{
    ScopeError error;
    std::string_view description;
    BusHealth health;
    ScopeErrorSeverity severity;
};

inline constexpr auto ScopeErrorTable = std::array {
    ScopeErrorTraits { ScopeError::Unavailable,
                       "no systemd user instance",
                       BusHealth::Unresponsive,
                       ScopeErrorSeverity::Expected },
    ScopeErrorTraits { ScopeError::Disconnected,
                       "the bus connection broke",
                       BusHealth::Unresponsive,
                       ScopeErrorSeverity::Fault },
    ScopeErrorTraits { ScopeError::TimedOut,
                       "systemd did not answer in time",
                       BusHealth::Unresponsive,
                       ScopeErrorSeverity::Fault },
    ScopeErrorTraits {
        ScopeError::Refused, "systemd refused the scope", BusHealth::Responsive, ScopeErrorSeverity::Fault },
    ScopeErrorTraits { ScopeError::UnknownProperty,
                       "systemd does not know a requested property",
                       BusHealth::Responsive,
                       ScopeErrorSeverity::Fault },
};

/// @return The row of ScopeErrorTable describing @p error.
[[nodiscard]] constexpr ScopeErrorTraits const& traitsOf(ScopeError error) noexcept
{
    return ScopeErrorTable[static_cast<std::size_t>(error)];
}

/// The seam over systemd's D-Bus API.
class ScopeBus
{
  public:
    virtual ~ScopeBus() = default;

    /// Asks systemd for a transient scope holding the request's process, and waits for its job.
    /// @param request  The scope to create.
    /// @param deadline How long the request and its job may take, together.
    /// @return Nothing once the job ended as `done`; otherwise why not.
    [[nodiscard]] virtual std::expected<void, ScopeError> startScope(ScopeRequest const& request,
                                                                     std::chrono::milliseconds deadline) = 0;
};

/// Connects a ScopeBus. Called on the placement's worker: at first use, and again after it broke.
using ScopeBusFactory = std::function<std::expected<std::unique_ptr<ScopeBus>, ScopeError>()>;

} // namespace vtpty
