// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Where `net` reports a failure it cannot return to a caller.
///
/// Almost everything here is `std::expected`, delivered to whoever asked. A few
/// places have no such caller — a multiplexed wait failing mid-sweep, for instance,
/// is nobody's operation — and those would otherwise fail silently.
///
/// This is a dependency-injection seam rather than a direct call into a logging
/// library so `net` stays free of Contour's `crispy` and can be consumed by other
/// projects unchanged. The default sink discards, so a caller that never sets one
/// pays only a null check.

#include <functional>
#include <string_view>

namespace net
{

/// Receives diagnostics that have no caller to return them to.
///
/// Invoked from whichever thread hit the condition — an implementation that
/// touches shared state must do its own synchronization. Never invoked with a
/// null target; @c setDiagnosticSink resets to the discarding default instead.
using DiagnosticSink = std::function<void(std::string_view message)>;

/// Installs the process-wide diagnostic sink.
///
/// A process-wide setter rather than a constructor parameter is a deliberate
/// exception to the configuration-at-construction rule: the reporting sites are
/// deep inside platform code reached through the @c EventSource interface, and
/// threading a sink through every one of them would put an observability concern
/// into signatures that have nothing else to do with it.
///
/// **Prefer installing once, before any loop runs** — it is a startup decision, and
/// treating it as one keeps the reporting order across threads predictable. It is
/// nevertheless safe to install at any time: the sink is held behind a shared
/// pointer, so a reporting thread keeps its target alive for the duration of the
/// call and a concurrent replacement cannot destroy the callable under it. The lock
/// is held only while reading or writing the pointer, never across the invocation,
/// so a slow sink does not serialize reporting threads.
/// @param sink The sink to install; an empty target restores the discarding default.
void setDiagnosticSink(DiagnosticSink sink);

/// Reports a diagnostic to the installed sink.
/// @param message The message; formatted by the caller, since the default sink
///        discards it and most callers never fire.
void reportDiagnostic(std::string_view message);

/// @return True if a non-discarding sink is installed. Check this before doing
///         formatting work whose only purpose is the message.
[[nodiscard]] bool hasDiagnosticSink() noexcept;

} // namespace net
