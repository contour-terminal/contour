// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `IAdmissionControl` — the policy that caps how many connections a server holds at once.
///
/// An accept loop consults it between accepts; a denied accept means the just-accepted socket is
/// closed immediately rather than queued, because a server that accepts what it cannot serve has
/// only moved the queue somewhere the client cannot see.
///
/// Imported from fastcached's `Net/IAdmissionControl.hpp` at
/// `0708dd54dc7ee72622c8c0783c2bd4a06f0e9b21`, and reshaped rather than renamed: upstream asked
/// `AllowAccept()` and then told `OnConnectionStarted()`, which is a check-then-act across two
/// calls, and ended a connection with a third call nothing paired with the first two.

#include <atomic>
#include <cstddef>
#include <optional>
#include <utility>

namespace core::net
{

class IAdmissionControl;

/// One admitted connection's hold on its policy's capacity; ending the lease gives the slot back.
///
/// **A lease rather than an `onConnectionEnded()` call, because a call is something a caller can
/// forget to make, or make twice.** Upstream's counter wrapped to `SIZE_MAX` on an end with no
/// matching start, after which every accept was refused for good. Here the slot is returned by a
/// destructor, exactly once, on every path out of the connection — an exception included.
///
/// Move-only. A moved-from lease holds nothing and gives nothing back. It must not outlive the
/// policy that granted it.
class AdmissionLease
{
  public:
    AdmissionLease(AdmissionLease const&) = delete;
    AdmissionLease& operator=(AdmissionLease const&) = delete;

    AdmissionLease(AdmissionLease&& other) noexcept: _policy(std::exchange(other._policy, nullptr)) {}

    AdmissionLease& operator=(AdmissionLease&& other) noexcept
    {
        if (this != &other)
        {
            end();
            _policy = std::exchange(other._policy, nullptr);
        }
        return *this;
    }

    /// Gives the slot back, unless this lease was moved from.
    ~AdmissionLease() { end(); }

  private:
    friend class IAdmissionControl;

    /// @param policy The policy whose slot this lease holds.
    explicit AdmissionLease(IAdmissionControl& policy) noexcept: _policy(&policy) {}

    /// Returns the slot to the policy, once.
    inline void end() noexcept;

    IAdmissionControl* _policy = nullptr;
};

/// Whether another connection may be admitted right now, decided and counted in one step.
///
/// **One atomic `tryAdmit()` rather than a question followed by a report.** Two accept loops
/// sharing a policy, capped at 100 with 99 in flight, would both be told "yes" by a separate
/// `allowAccept()` before either reported its start, and the server would hold 101: a cap
/// exceeded under exactly the load it exists for.
class IAdmissionControl
{
  public:
    IAdmissionControl() = default;
    virtual ~IAdmissionControl() = default;

    IAdmissionControl(IAdmissionControl const&) = delete;
    IAdmissionControl& operator=(IAdmissionControl const&) = delete;
    IAdmissionControl(IAdmissionControl&&) = delete;
    IAdmissionControl& operator=(IAdmissionControl&&) = delete;

    /// Admits one connection if the policy allows it, and counts it in the same step.
    ///
    /// Safe to call from any thread. Of two callers racing for the last slot exactly one is
    /// admitted.
    /// @return The lease that holds the slot for as long as the connection lives, or nothing if
    ///         the policy refuses — in which case the caller closes the connection rather than
    ///         queueing it.
    [[nodiscard]] virtual std::optional<AdmissionLease> tryAdmit() noexcept = 0;

  protected:
    /// Builds the lease an implementation hands out from @c tryAdmit once it has counted it.
    /// @return A lease that calls @c release exactly once when it ends.
    [[nodiscard]] AdmissionLease grant() noexcept { return AdmissionLease { *this }; }

  private:
    friend class AdmissionLease;

    /// Gives back one slot a lease held. Called exactly once per lease @c grant produced, from
    /// whichever thread ends the lease.
    virtual void release() noexcept = 0;
};

inline void AdmissionLease::end() noexcept
{
    if (auto* const policy = std::exchange(_policy, nullptr); policy != nullptr)
        policy->release();
}

/// The default policy: a cap on concurrent connections. Thread-safe.
///
/// **The cap is fixed at construction.** Upstream's `SetMax` wrote a plain field that accepting
/// threads read — a data race — and changing a cap under live leases has no answer this class
/// could give for the connections already over it. A reload builds a new policy for the loops
/// that start after it.
class CountingAdmissionControl final: public IAdmissionControl
{
  public:
    /// @param maxConcurrent The connection cap; 0 means unlimited.
    explicit CountingAdmissionControl(std::size_t maxConcurrent = 0) noexcept: _max(maxConcurrent) {}

    /// @copydoc IAdmissionControl::tryAdmit
    ///
    /// A compare-and-swap on the in-flight count: the check against the cap and the increment are
    /// one step, so no second caller can slip between them.
    [[nodiscard]] std::optional<AdmissionLease> tryAdmit() noexcept override
    {
        if (_max == 0)
        {
            _inFlight.fetch_add(1, std::memory_order_acq_rel);
            return grant();
        }
        auto current = _inFlight.load(std::memory_order_acquire);
        while (current < _max)
        {
            if (_inFlight.compare_exchange_weak(
                    current, current + 1, std::memory_order_acq_rel, std::memory_order_acquire))
                return grant();
        }
        return std::nullopt;
    }

    /// @return How many connections are in flight: leases granted and not yet ended.
    [[nodiscard]] std::size_t inFlight() const noexcept { return _inFlight.load(std::memory_order_acquire); }

  private:
    void release() noexcept override { _inFlight.fetch_sub(1, std::memory_order_acq_rel); }

    std::size_t _max;
    std::atomic<std::size_t> _inFlight { 0 };
};

} // namespace core::net
