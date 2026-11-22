// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace vtextension
{

/// The extension world a module speaks: which host functions its engine's runtime is created with, which
/// imports and exports a module loaded on it may have, and which manifest placement it must declare.
///
/// Each world has its own engine: a module of one world never loads on the other's.
enum class World : uint8_t
{
    StatusLine, ///< `terminal-extensions:statusline`: an extension drawn in a pane's status line.
    Daemon,     ///< `terminal-extensions:daemon`: an extension the daemon runs with no window attached.
};

} // namespace vtextension
