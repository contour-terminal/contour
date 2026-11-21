// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace vtextension
{

/// When a daemon-world extension is started: the runtime's own spelling of a configuration's `autostart`.
enum class ExtensionAutostart : uint8_t
{
    OnDemand,      ///< When something asks for it: an action, a served tab, or runs left in its store.
    AtDaemonStart, ///< As the daemon starts, with no window attached.
};

} // namespace vtextension
