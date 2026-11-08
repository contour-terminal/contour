// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace vtpty
{

/// Whether a child takes its PTY slave as its controlling terminal.
enum class ControllingTerminal : std::uint8_t
{
    Skip,    ///< A sandbox such as flatpak forbids TIOCSCTTY.
    Acquire, ///< TIOCSCTTY, as login_tty() does.
};

} // namespace vtpty
