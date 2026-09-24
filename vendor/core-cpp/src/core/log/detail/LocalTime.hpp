// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <ctime>

/// @file
/// The local-time conversion the timestamp field needs, asked once per platform.
///
/// Private: this header is in no file set, so it is not part of core::log's public API. It exists
/// only so the one declaration is shared between LogSink.cpp and the two implementations that
/// answer it -- POSIX spells the reentrant conversion `localtime_r` and Windows `localtime_s`,
/// with their two arguments the other way round, and that is an implementation difference rather
/// than an `#ifdef` inside the code that formats a log line (`.agent/rules/platform.md`).

namespace core::log::detail
{

/// Breaks @p time down into LOCAL time.
///
/// @param time The time point to convert.
/// @return Its broken-down fields, or a zeroed `std::tm` if the platform could not convert it.
[[nodiscard]] std::tm localTime(std::time_t time) noexcept;

} // namespace core::log::detail
