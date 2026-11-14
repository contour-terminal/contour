// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A diagnostic that shows where the OS swallows a key press before Qt sees it. Installed only when the
/// gui.input log is enabled at startup; a no-op off macOS.
///
/// The trace logs a key event's characters only while Command or Control is held, which is all the diagnosis
/// needs, so what is typed into a password prompt is never logged. It is installed at startup only: enabling
/// gui.input later needs a restart, while disabling it silences the trace at once.

#include <functional>
#include <memory>
#include <string>

namespace contour::platform
{

/// A live native key trace. Construction installs it, destruction removes it.
class INativeKeyTrace
{
  public:
    INativeKeyTrace() = default;
    INativeKeyTrace(INativeKeyTrace const&) = delete;
    INativeKeyTrace& operator=(INativeKeyTrace const&) = delete;
    INativeKeyTrace(INativeKeyTrace&&) = delete;
    INativeKeyTrace& operator=(INativeKeyTrace&&) = delete;
    virtual ~INativeKeyTrace() = default;
};

/// Receives one trace line.
using NativeKeyTraceSink = std::function<void(std::string const&)>;

/// Installs the trace: on macOS an NSEvent local monitor that reports every key event to @p sink and never
/// consumes one; elsewhere nothing.
/// @pre GUI (main) thread.
/// @param sink Receives each line.
/// @return The installed trace; keep it alive for as long as it should run.
[[nodiscard]] std::unique_ptr<INativeKeyTrace> makeNativeKeyTrace(NativeKeyTraceSink sink);

} // namespace contour::platform
