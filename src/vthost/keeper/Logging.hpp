// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The PTY keeper's log category. The keeper process logs to the daemon's log file (the verb that starts it
/// installs the destination, from its own command line: the keeper never reads the options record).

#include <core/log/LogStore.hpp>

namespace vthost::keeper
{

/// Enabled by default, banner-grade like `vthost.daemon`: what the keeper did to the daemon it keeps.
auto inline const keeperLog = core::log::Category("vthost.keeper",
                                                  "The PTY keeper: daemon launches, restarts, and the "
                                                  "settings it was given.",
                                                  core::log::Category::State::Enabled);

} // namespace vthost::keeper
