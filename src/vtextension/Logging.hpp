// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// vtextension's log categories. A failure goes to the always-on `error`; what is a state an extension is
/// normally in goes here, for whoever asks with `LOG=vtextension.*`.

#include <core/log/LogStore.hpp>

namespace vtextension
{

/// Where a daemon-scoped extension stands: waiting for the user's consent, or denied it.
auto inline const daemonExtensionLog = core::log::Category(
    "vtextension.daemon", "Daemon-scoped extensions: waiting for the user's consent, or denied it.");

} // namespace vtextension
