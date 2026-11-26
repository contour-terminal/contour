// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Why Contour runs no extension host, said once: a host it could not start, or one it refused at the
/// handshake. Each reason is logged the first time it comes up, never again for that backend -- a reason
/// stays as long as the installation does, and every later attempt would only repeat it.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

#include <vtextension/wasm/remote/proto/LockdownSteps.hpp>
#include <vtextension/wasm/remote/proto/Pdu.hpp>

namespace vtextension::wasm::remote
{

/// Why no extension host serves.
enum class HostRefusal : uint8_t
{
    HelperNotFound,   ///< The host executable is missing, or cannot be executed.
    OutOfResources,   ///< The system is out of memory, processes or handles.
    Unsupported,      ///< The system lacks what starting a host takes.
    LaunchRefused,    ///< The system refused to start it for another reason.
    HelperNotGranted, ///< Its directory does not let it run contained, and only its installer can grant that.
    ProtocolVersion,  ///< It speaks another protocol version than Contour.
    LockdownFailed,   ///< It failed to confine itself.
    Unsandboxed,      ///< It runs without a sandbox, which Contour requires.
};

/// The last reason: what every table and loop over the reasons ends at. refusalSource()'s switch, which has
/// no default, keeps it the last: a reason added after it fails to compile until this names it.
inline constexpr auto LastHostRefusal = HostRefusal::Unsandboxed;

/// Where a reason's line comes from.
enum class RefusalSource : uint8_t
{
    None,      ///< Nowhere: the value names no reason.
    Launch,    ///< The launcher's table (launchErrors()): a host that could not be started.
    Handshake, ///< The handshake: a host that started and was refused.
};

/// @param refusal A reason, or any value of its type.
/// @return Where its line comes from; `None` if @p refusal names no reason.
[[nodiscard]] constexpr RefusalSource refusalSource(HostRefusal refusal) noexcept
{
    switch (refusal)
    {
        case HostRefusal::HelperNotFound:
        case HostRefusal::OutOfResources:
        case HostRefusal::Unsupported:
        case HostRefusal::LaunchRefused:
        case HostRefusal::HelperNotGranted: return RefusalSource::Launch;
        case HostRefusal::ProtocolVersion:
        case HostRefusal::LockdownFailed:
        case HostRefusal::Unsandboxed: return RefusalSource::Handshake;
    }
    return RefusalSource::None;
}

static_assert(refusalSource(LastHostRefusal) != RefusalSource::None);
static_assert(refusalSource(static_cast<HostRefusal>(std::to_underlying(LastHostRefusal) + 1))
                  == RefusalSource::None,
              "LastHostRefusal must name the last reason");

/// Whether a reason a host was refused for would hold for the next host a connector started.
enum class RefusalPersistence : uint8_t
{
    Transient, ///< It may not: the next host is started.
    Lasting,   ///< It would: a property of the system or the build, which every later host shares.
};

/// A reason, and whether it lasts.
struct RefusalPersistenceRow
{
    HostRefusal refusal;            ///< The reason.
    RefusalPersistence persistence; ///< Whether a host started again would be refused for it as well.
};

/// Every reason, one row each, at its enumerator's index. A host refused at the handshake for a lasting
/// reason is the last its connector starts (IHostConnector::refused()).
inline constexpr auto RefusalPersistenceRows = std::array {
    RefusalPersistenceRow { .refusal = HostRefusal::HelperNotFound,
                            .persistence = RefusalPersistence::Transient },
    RefusalPersistenceRow { .refusal = HostRefusal::OutOfResources,
                            .persistence = RefusalPersistence::Transient },
    RefusalPersistenceRow { .refusal = HostRefusal::Unsupported,
                            .persistence = RefusalPersistence::Transient },
    RefusalPersistenceRow { .refusal = HostRefusal::LaunchRefused,
                            .persistence = RefusalPersistence::Transient },
    // Said once; a reinstall may grant it while Contour runs.
    RefusalPersistenceRow { .refusal = HostRefusal::HelperNotGranted,
                            .persistence = RefusalPersistence::Transient },
    // The host installed with Contour speaks Contour's version; another one stays installed.
    RefusalPersistenceRow { .refusal = HostRefusal::ProtocolVersion,
                            .persistence = RefusalPersistence::Lasting },
    RefusalPersistenceRow { .refusal = HostRefusal::LockdownFailed,
                            .persistence = RefusalPersistence::Lasting },
    // Refused only where a sandbox is required, and a system without one does not grow one.
    RefusalPersistenceRow { .refusal = HostRefusal::Unsandboxed, .persistence = RefusalPersistence::Lasting },
};

/// @return Whether every reason has its row, at its enumerator's index.
[[nodiscard]] consteval bool everyRefusalHasAPersistenceRow() noexcept
{
    if (RefusalPersistenceRows.size() != std::size_t { std::to_underlying(LastHostRefusal) } + 1)
        return false;
    for (auto const index: std::views::iota(std::size_t { 0 }, RefusalPersistenceRows.size()))
        if (std::to_underlying(RefusalPersistenceRows.at(index).refusal) != index)
            return false;
    return true;
}
static_assert(everyRefusalHasAPersistenceRow());

/// @param refusal A reason.
/// @return Whether it would hold for a host started again.
[[nodiscard]] constexpr RefusalPersistence refusalPersistence(HostRefusal refusal) noexcept
{
    return RefusalPersistenceRows.at(std::to_underlying(refusal)).persistence;
}

/// @param refusal Why no extension host serves.
/// @return The line logged for it, user-readable, naming no engine, never empty. A host that could not be
///         started says what the launcher says of its failure (launchErrors()), in one sentence for every
///         such reason.
[[nodiscard]] std::string refusalLine(HostRefusal refusal);

/// @param refusal Why no extension host serves.
/// @param failure The step of its confinement a host refused as `LockdownFailed` named in its Hello, and the
///        code it stated.
/// @return refusalLine(@p refusal); for `LockdownFailed` with a step, naming that step and what its code says
///         as the confinement's own table phrases them (proto::lockdownFailureText()).
[[nodiscard]] std::string refusalLine(HostRefusal refusal, proto::LockdownFailure failure);

/// @param step A step of its confinement a host's Hello said this system lacks.
/// @return The line logged for it, user-readable, naming no engine: the host runs confined, without it.
[[nodiscard]] std::string unavailableStepLine(proto::LockdownStep step);

/// @param failure The step of its confinement a host's Hello said failed, and its code.
/// @return Why the host is refused: `LockdownFailed`, which lasts, but for a failure a host started again may
///         not meet (a row of TransientLockdownFailures in HostRefusals.cpp), which is refused for that.
[[nodiscard]] HostRefusal lockdownFailureRefusal(proto::LockdownFailure failure) noexcept;

/// How a logged line weighs.
enum class LineSeverity : uint8_t
{
    Warning, ///< Extensions run, though not quite as they should.
    Error,   ///< Extensions do not run.
};

/// Logs each reason the first time it comes up. Thread-safe; the reasons seen are kept wait-free.
class HostRefusalLog
{
  public:
    /// Writes one logged line, as heavy as @p severity says.
    using Writer = std::function<void(LineSeverity severity, std::string_view line)>;

    /// @param writer Where the lines go: logWriter() for Contour's log, a recorder in a test.
    explicit HostRefusalLog(Writer writer);

    /// Logs @p refusal's line, unless it was logged already.
    /// @param refusal Why no host serves.
    void refused(HostRefusal refusal);

    /// Logs @p refusal's line naming @p failure (refusalLine()), unless @p refusal was logged already.
    /// @param refusal Why no host serves.
    /// @param failure The step of its confinement the host named, and its code.
    void refused(HostRefusal refusal, proto::LockdownFailure failure);

    /// Warns that hosts run without @p step, which their system lacks (unavailableStepLine()), unless that
    /// was logged already: once, though every host started says it. `None` is no step, and logs nothing.
    /// @param step The step a host's Hello said it went without.
    void unavailable(proto::LockdownStep step);

  private:
    Writer _writer;
    /// A bit per reason logged, at its enumerator's value.
    std::atomic<uint32_t> _logged { 0 };
    /// A bit per unavailable step logged, at its enumerator's value.
    std::atomic<uint32_t> _unavailable { 0 };
};

/// @return A writer to Contour's log: an error to its error log, a warning to `vtextension.warning`, which is
///         on unless turned off.
[[nodiscard]] HostRefusalLog::Writer logWriter();

} // namespace vtextension::wasm::remote
