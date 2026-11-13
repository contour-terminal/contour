// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What the tests of a NativeController reach inside it with (@see NativeController's friend declaration):
/// the reactor-owned client, and the handlers a daemon's PDUs reach, called as the reactor would.

#include <contour/remote/NativeController.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

#include <vthost/client/NativeClient.hpp>
#include <vthost/proto/Pdu.hpp>

namespace contour::remote
{

/// Reaches the reactor-owned client of a controller (@see NativeController's friend declaration).
struct NativeControllerTestAccess
{
    /// Runs @p action on the reactor thread with the controller's current client, and waits for it.
    template <typename F>
    static void onReactor(contour::remote::NativeController& controller, F action)
    {
        auto done = std::promise<void> {};
        controller._reactor.post([&] {
            action(*controller._client);
            done.set_value();
        });
        done.get_future().wait();
    }

    /// Waits until the controller's reactor thread ran everything posted to it before this call.
    /// @param timeout How long to wait.
    /// @return Whether it did within @p timeout.
    [[nodiscard]] static bool drainReactor(contour::remote::NativeController& controller,
                                           std::chrono::milliseconds timeout)
    {
        // Shared, so a reactor that runs it after a timed-out wait still sets a live promise.
        auto const drained = std::make_shared<std::promise<void>>();
        controller._reactor.post([drained] { drained->set_value(); });
        return drained->get_future().wait_for(timeout) == std::future_status::ready;
    }

    /// Makes the reactor's run end with an exception, as a decode error escaping the root task would.
    static void killReactor(contour::remote::NativeController& controller)
    {
        controller._reactor.post([] { throw std::runtime_error("reactor died"); });
    }

    /// Makes the connection up one that has pushed no layout frame yet, its reconnect having lost @p lost,
    /// and has the daemon end a push with no frame in it (one a move request alone caused).
    static void endFramelessPushOfFreshConnection(contour::remote::NativeController& controller,
                                                  std::vector<uint64_t> lost)
    {
        onReactor(controller, [&](auto& /*client*/) {
            {
                auto const lock = std::lock_guard { controller._mutex };
                controller._connectionLayouts = NativeController::ConnectionLayouts::Awaited;
                controller._lostWorkspaces = std::move(lost);
            }
            controller.onLayoutBatchEnd(vthost::proto::LayoutBatchEnd {});
        });
    }

    /// Has the daemon push @p layout and end the push, on the reactor, as it would.
    static void pushAndEnd(contour::remote::NativeController& controller,
                           vthost::proto::LayoutState const& layout)
    {
        onReactor(controller, [&](auto& /*client*/) {
            controller.onLayout(layout);
            controller.onLayoutBatchEnd(vthost::proto::LayoutBatchEnd {});
        });
    }

    /// Has the daemon send a bell of @p session, then state @p role, in one turn of the reactor: no GUI event
    /// is processed between the two.
    static void bellThenRole(contour::remote::NativeController& controller,
                             uint64_t session,
                             vthost::proto::RaiseRole role)
    {
        onReactor(controller, [&](auto& /*client*/) {
            auto screen = vthost::client::RemoteScreen {};
            screen.session = session;
            controller.onSessionEvent(screen, vthost::proto::SessionBell { .session = session });
            controller.onRaiseRole(role);
        });
    }

    /// Has the daemon state @p role, then send @p summary, on the reactor, as it would.
    static void roleThenSummary(contour::remote::NativeController& controller,
                                vthost::proto::RaiseRole role,
                                vthost::proto::PaneSummary const& summary)
    {
        onReactor(controller, [&](auto& /*client*/) {
            controller.onRaiseRole(role);
            controller.onPaneSummary(summary);
        });
    }

    /// @return How many workspaces the controller holds metadata of (WorkspaceMeta), the removed ones pruned.
    [[nodiscard]] static std::size_t workspaceMetaCount(contour::remote::NativeController const& controller)
    {
        auto const lock = std::lock_guard { controller._mutex };
        return controller._workspaceMeta.size();
    }

    /// Ends the current client's connection and, in the same reactor turn, closes @p session's pane.
    static void closeWhileEnding(contour::remote::NativeController& controller, uint64_t session)
    {
        onReactor(controller, [&](auto& client) {
            client.detach();
            controller.closePaneOnReactor(session);
        });
    }
};

} // namespace contour::remote
