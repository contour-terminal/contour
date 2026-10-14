// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace vtparser
{

/// Whether the parser may hand runs of bytes to its listener in bulk.
///
/// The bulk scanners (text, CSI parameters, DCS and APC payload) are shortcuts past the state
/// machine for bytes it would only pass on, and they must not change a single event. Disabled
/// routes every byte through the state machine, which makes it the reference those shortcuts are
/// tested against.
enum class BulkScan : uint8_t
{
    Enabled,  ///< The default: runs of bytes skip the state machine where it would decide nothing.
    Disabled, ///< Every byte goes through the state machine.
};

} // namespace vtparser
