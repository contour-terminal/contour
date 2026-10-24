// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `LoopControlBridge` — how a WebAssembly extension's control call, made on its worker thread, is served by
/// the daemon's control API on the daemon's loop thread.

#include <core/net/EventLoop.hpp>

#include <chrono>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include <vtcontrol/Catalog.hpp>
#include <vtcontrol/ControlApi.hpp>
#include <vtcontrol/ControlError.hpp>

namespace vthost::control
{

/// Who serves an extension's control call, and as whom: resolved on the loop thread, when the call is
/// served, because the API and the extension's pane may be gone by then.
struct ExtensionCaller
{
    vtcontrol::ControlApi& api; ///< What serves the call.
    std::string pane;           ///< The extension's pane, `pane:<uuid>`: the call's caller.
};

/// Resolves an ExtensionCaller on the loop thread; nothing when the call can no longer be served (the
/// extension's pane is closed, the host is going).
using ExtensionCallerResolver = std::function<std::optional<ExtensionCaller>()>;

/// Serves a control call from a thread that is not the loop's -- an extension's worker -- on the loop, and
/// waits for its reply.
///
/// **Threads.** call() is made on any thread but the loop's; it posts the call to the loop, where the
/// resolver and `vtcontrol::dispatch` run, and blocks the calling thread on the reply. Nothing of the call's
/// state is shared but the reply's slot, a promise both sides own: the loop writes the reply there, and the
/// worker reads it once it is there.
///
/// **Timeouts.** A call that is not answered within its timeout -- the rest of the guest's deadline --
/// returns `failed` "timed out". It is not cancelled: the loop may still serve it later, and then writes its
/// reply into the slot nobody reads any more, which is released with the job. That discarded reply reaches no
/// guest memory: the worker lowers a reply into its guest only from what call() returns, after checking that
/// the guest's deadline has not passed (vtextension's callControlImport()).
///
/// **Policy.** The call is dispatched as `vtcontrol::Caller::Extension` with the extension's granted set as
/// the context's grants, so 7a's dispatch-level checks (`SocketOnly` is `unsupported`, a missing grant
/// `forbidden`) fence the call a second time behind the import's. The import's own refusals -- an undeclared
/// permission, a `pane.send` that waits -- are not repeated here.
class LoopControlBridge
{
  public:
    /// @param loop The loop the control API runs on. Borrowed: it must outlive every call().
    /// @param resolve Who serves a call, resolved on @p loop's thread.
    LoopControlBridge(core::net::EventLoop& loop, ExtensionCallerResolver resolve);

    /// Serves one call on the loop and waits for its reply. Must not be called on the loop's thread, which
    /// would wait for itself (a precondition violation).
    /// @param method The method's wire name.
    /// @param params Its request JSON.
    /// @param granted What the extension is granted.
    /// @param timeout The longest to wait for the reply.
    /// @return The reply JSON; `unsupported` when the call can no longer be served, `failed` "timed out" when
    ///         @p timeout passed first, or the call's own error.
    [[nodiscard]] std::expected<std::string, vtcontrol::ControlError> call(
        std::string_view method,
        std::string_view params,
        vtcontrol::ControlGrants granted,
        std::chrono::milliseconds timeout) const;

  private:
    core::net::EventLoop* _loop;
    ExtensionCallerResolver _resolve;
};

} // namespace vthost::control
