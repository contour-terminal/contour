// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/platform/Clock.hpp>

namespace vtbackend
{

/// A borrowed steady clock that cannot be built from a temporary: the steady-clock twin of
/// core::platform::WallClockRef, for the same reason.
///
/// A deleted rvalue overload on a type that STORES the clock only refuses a direct temporary: whoever
/// forwards the clock re-binds it as a named lvalue, which the ordinary overload accepts. Carried as a
/// value, the borrow keeps its refusal through every hop -- and an aggregate holding one refuses a
/// temporary in brace-initialization too, which a bare `IClock const&` member would bind without
/// complaint.
///
/// Implicitly constructible, and implicitly convertible back to the clock, so a named clock goes in and
/// an `IClock const&` comes out wherever one is expected.
class SteadyClockRef
{
  public:
    /// Borrows a steady clock.
    /// @param clock The clock to borrow. It must outlive every holder of this reference;
    ///        core::platform::defaultSteadyClock() has static storage and any named object will do.
    SteadyClockRef(core::platform::IClock const& clock) noexcept: _clock { &clock } {}

    /// Refuses a temporary, which would dangle -- the entire reason this type exists.
    SteadyClockRef(core::platform::IClock const&&) = delete;

    /// @return The borrowed clock, for a caller that needs the interface itself.
    [[nodiscard]] core::platform::IClock const& get() const noexcept { return *_clock; }

    /// @return What the borrowed clock says the time is, so a holder need not unwrap it.
    [[nodiscard]] core::platform::SteadyTimePoint now() const noexcept { return _clock->now(); }

    /// @return The borrowed clock, wherever an `IClock const&` is expected.
    operator core::platform::IClock const&() const noexcept { return *_clock; }

  private:
    core::platform::IClock const* _clock;
};

} // namespace vtbackend
