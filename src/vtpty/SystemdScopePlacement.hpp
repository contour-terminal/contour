// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/PlacementBreaker.hpp>
#include <vtpty/ProcessCgroup.hpp>
#include <vtpty/ProcessPlacement.hpp>
#include <vtpty/ScopeBus.hpp>

#include <core/platform/Clock.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace vtpty
{

/// The name SystemdScopePlacement's worker thread carries, at most 15 characters as Linux allows.
inline constexpr auto WorkerThreadName = "scope-placement";

/// How SystemdScopePlacement places children.
struct SystemdScopeConfig
{
    /// Each scope is named `<unitPrefix>-<contour pid>-<child pid>-<n>.scope`.
    std::string unitPrefix = "contour-session";

    /// The slice the scopes go into: where desktops put applications, and what systemd-oomd watches.
    std::string slice = "app.slice";

    /// How long one request may take before its child is released unplaced.
    std::chrono::milliseconds deadline { 1000 };

    /// How long an unresponsive bus is left alone. @see PlacementBreaker.
    core::platform::SteadyDuration breakerCooldown = std::chrono::seconds { 30 };

    /// A memory ceiling for every scope. Tests only: a user-facing limit is not a feature yet.
    std::optional<MemoryLimit> memoryLimit {};

    /// The oom_score_adj each child sets for itself, if any. @see ProcessPlacement::childOomScoreAdjust.
    std::optional<int> childOomScoreAdjust {};
};

/// What became of a session process.
enum class PlacementStatus : std::uint8_t
{
    Placed,        ///< In a scope of its own before it ran.
    PlacedUnheard, ///< In a scope of its own before it ran, though systemd's answer did not arrive in time.
    Deferred,      ///< Released unplaced; it is moved into a scope of its own once systemd answers again.
    PlacedLate,    ///< Moved into a scope of its own while it ran. What it started before stayed behind.
    Unplaced,      ///< Released unplaced for good.
    Ended,         ///< Exited while it waited to be placed late.
};

/// One thing SystemdScopePlacement tells about a session process.
struct PlacementReport
{
    /// The session process.
    int pid = 0;

    /// What became of it.
    PlacementStatus status = PlacementStatus::Placed;

    /// Why it was released unplaced, or why systemd's answer went unheard; nothing for Placed and Ended.
    std::optional<ScopeError> reason = std::nullopt;

    [[nodiscard]] bool operator==(PlacementReport const&) const = default;
};

/// Tells what became of a session process. Called on the placement's worker.
using PlacementReporter = std::function<void(PlacementReport const&)>;

/// How a PlacementReport reads in the log.
struct PlacementLogLine
{
    ScopeErrorSeverity severity; ///< Fault where a user is to see it.
    std::string text;            ///< The line, naming the process and why.
};

/// @param report What became of a session process.
/// @return How it reads in the log: a fault where it was released unplaced, placed after such a
///         release, or placed without systemd's answer, for a reason a user should hear of; expected
///         otherwise.
[[nodiscard]] PlacementLogLine describe(PlacementReport const& report);

/// The PlacementReporter Contour uses: faults go to the error log, everything else to
/// `pty.placement`.
/// @param report What became of a session process.
void logPlacement(PlacementReport const& report);

/// Places every child in a transient systemd scope of its own, with OOMPolicy=continue, so that an
/// out-of-memory kill inside a session stops neither Contour's unit nor the rest of that session.
///
/// One worker thread owns the bus -- sd-bus connections are not thread-safe -- and it starts with
/// the first child, so a placement nobody uses costs nothing. Every child is released whatever
/// happens: placed, refused, timed out, skipped by the breaker, or still queued at destruction.
///
/// A child released unplaced because the bus fell silent is not forgotten. Once the breaker lets
/// requests through again, the worker moves it into a scope of its own while it runs; what it started
/// in the meantime stays where it is.
class SystemdScopePlacement final: public ProcessPlacement
{
  public:
    /// @param config     How to place children.
    /// @param connectBus Connects the bus; called on the worker, at first use and after it broke.
    /// @param readCgroup Tells where the kernel says a child runs; asked on the worker, once a
    ///                   request's answer did not arrive in time.
    /// @param report     Tells what became of each child; called on the worker.
    /// @param clock      The clock the breaker reads; must outlive this placement.
    SystemdScopePlacement(SystemdScopeConfig config,
                          ScopeBusFactory connectBus,
                          ProcessCgroupReader readCgroup,
                          PlacementReporter report,
                          core::platform::IClock const& clock);

    /// Stops the worker -- after the request in flight, if any -- and releases every queued child.
    ~SystemdScopePlacement() override;

    SystemdScopePlacement(SystemdScopePlacement const&) = delete;
    SystemdScopePlacement& operator=(SystemdScopePlacement const&) = delete;
    SystemdScopePlacement(SystemdScopePlacement&&) = delete;
    SystemdScopePlacement& operator=(SystemdScopePlacement&&) = delete;

    void placeThenRelease(ParkedChild child) noexcept override;

    [[nodiscard]] std::optional<int> childOomScoreAdjust() const noexcept override
    {
        return _config.childOomScoreAdjust;
    }

  private:
    /// The process a request asks systemd to move.
    struct ScopeTarget
    {
        int pid;   ///< The process.
        int pidfd; ///< A pidfd for it, or -1.
    };

    /// A child released unplaced, waiting for the breaker to let a late placement through.
    struct DeferredChild
    {
        UnplacedChild child; ///< The child, with the pidfd that tells whether its pid still names it.
        ScopeError reason;   ///< Why it was released unplaced.
    };

    void startWorker();
    void run();
    /// Lets the child go that placing threw over, and tells why: @p child where it was a queued one, else
    /// the deferred child the late pass was asking about, which is then not asked about again. On the
    /// worker.
    void letGoAfterFailure(std::optional<ParkedChild>& child, char const* what) noexcept;
    /// Waits until a child is queued, a deferred child is due, or the placement stops. Called with
    /// @p lock held, on the worker.
    void awaitWork(std::unique_lock<std::mutex>& lock);
    /// @return Whether a deferred child waits and the breaker lets a request through.
    [[nodiscard]] bool isLatePlacementDue() const noexcept;
    void place(ParkedChild& child);
    /// Places the oldest deferred child, after letting go of those that exited.
    void placeLate();
    /// Releases @p child unplaced, and keeps it for a late placement where systemd is expected back and
    /// the child's pid can be trusted.
    void defer(ParkedChild& child, ScopeError reason);
    /// How systemd's answer to a request that placed its child arrived.
    enum class Answer : std::uint8_t
    {
        InTime,  ///< Before the deadline.
        Unheard, ///< After it, or never: the kernel says the child runs in its scope all the same.
    };

    /// Asks systemd for a scope for @p target, connecting first where there is no bus, and tells the
    /// breaker how it went: an unheard answer as the silence it was.
    [[nodiscard]] std::expected<Answer, ScopeError> attempt(ScopeTarget target);
    [[nodiscard]] std::expected<Answer, ScopeError> request(ScopeTarget target, ScopeProtocol protocol);
    /// @return Whether the kernel says process @p pid runs in @p unit's cgroup.
    [[nodiscard]] bool runsIn(int pid, std::string_view unit) const;
    void report(int pid, PlacementStatus status, std::optional<ScopeError> reason) const;

    SystemdScopeConfig _config;
    ScopeBusFactory _connectBus;
    ProcessCgroupReader _readCgroup;
    PlacementReporter _report;
    core::platform::IClock const& _clock;
    std::unique_ptr<ScopeBus> _bus;
    /// Current until systemd turns it down, then Legacy for as long as this connection lasts.
    ScopeProtocol _protocol = ScopeProtocol::Current;
    PlacementBreaker _breaker;
    std::uint64_t _sequence = 0;
    std::deque<DeferredChild> _deferred; ///< Oldest first; the worker's alone.
    /// The deferred child the late pass is asking systemd about, the oldest; nothing outside that request.
    /// The worker's alone. @see letGoAfterFailure
    std::optional<int> _askingLate;

    /// Whether the worker is to keep taking children off the queue.
    enum class WorkerState : std::uint8_t
    {
        Running,
        Stopping,
    };

    // A std::thread and a state rather than a std::jthread: this file is built on Linux only, and a
    // Linux build against libc++ before 20 has std::jthread only behind -fexperimental-library, which
    // the build passes only where its probe in the top-level CMakeLists.txt finds the flag works.
    std::mutex _mutex;
    std::condition_variable _wakeup;
    std::deque<ParkedChild> _queue;
    WorkerState _workerState = WorkerState::Running;
    std::once_flag _workerStarted;
    std::thread _worker;
};

} // namespace vtpty
