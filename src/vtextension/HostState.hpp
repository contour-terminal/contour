// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>

#include <vtextension/Event.hpp>
#include <vtextension/IHostServices.hpp>

namespace vtextension
{

/// Records what an event announces into the state an extension's `state` call answers with.
///
/// A user variable updates its entry, or adds one while fewer than @p maxUserVars are known; a name
/// beyond that is dropped, so a program cannot grow the state without bound by inventing names.
/// Timers and replies announce no state.
/// @param state The state to update.
/// @param event The event.
/// @param maxUserVars How many distinct user variables the state holds. @see ExtensionLimits::maxUserVars
void recordEvent(StateSnapshot& state, Event const& event, std::size_t maxUserVars);

} // namespace vtextension
