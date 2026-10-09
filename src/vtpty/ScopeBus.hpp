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

/// Which generation of systemd's transient-scope interface a request speaks.
enum class ScopeProtocol : std::uint8_t
{
    /// systemd before 253: `PIDs=[pid]`, and no `OOMPolicy=`, which scopes did not have -- nor need,
    /// as they did not react to an out-of-memory kill at all.
    Legacy,
    /// systemd 253 and later: `PIDFDs=[pidfd]` where there is a pidfd, and `OOMPolicy=continue`.
    Current,
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
    ScopeProtocol protocol;                 ///< Which properties the request may use.
    int pid;                                ///< The process to move.
    int pidfd;                              ///< A pidfd for it, or -1; used by ScopeProtocol::Current only.
    std::optional<MemoryLimit> memoryLimit; ///< A ceiling for the scope, if any.
};

/// Why a scope was not created.
enum class ScopeError : std::uint8_t
{
    Unavailable,  ///< No user bus, or no systemd user instance on it.
    Disconnected, ///< The bus connection broke.
    TimedOut,     ///< No answer before the deadline.
    Refused,      ///< systemd answered with an error, or the job did not end as `done`.
    Unsupported,  ///< systemd or the bus cannot take part of the request: speak ScopeProtocol::Legacy.
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
    ScopeErrorTraits { ScopeError::Unsupported,
                       "systemd or the bus cannot take part of the request",
                       BusHealth::Responsive,
                       ScopeErrorSeverity::Fault },
};

static_assert(
    [] {
        auto index = std::size_t { 0 };
        for (auto const& row: ScopeErrorTable)
            if (static_cast<std::size_t>(row.error) != index++ || row.description.empty())
                return false;
        return index == static_cast<std::size_t>(ScopeError::Unsupported) + 1;
    }(),
    "ScopeErrorTable holds one described row per ScopeError, in enumerator order");

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
