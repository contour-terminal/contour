// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The process id the `[PID]` field prints, asked once per platform.
///
/// Private: this header is in no file set, so it is not part of core::log's public API. It exists
/// only so the one declaration is shared between LogSink.cpp and the two implementations that
/// answer it -- an operating-system difference is an implementation, never an `#ifdef` inside the
/// code that uses it (`.agent/rules/platform.md`).

namespace core::log::detail
{

/// @return The current process id, as a plain number.
[[nodiscard]] int processId() noexcept;

} // namespace core::log::detail
