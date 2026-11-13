// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Asks in a window's dialog whether to pin a daemon met for the first time.

#include <contour/askpass/AskpassPromptRouter.hpp>
#include <contour/askpass/AskpassPromptSink.hpp>

#include <core/async/Task.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QObject>
#include <QtCore/QThread>

#include <functional>
#include <memory>
#include <string>
#include <utility>

#include <vthost/KnownDaemons.hpp>

namespace contour::askpass
{

/// Puts the pin confirmation (AskpassPromptKind::PinConfirm) in front of the user in one window.
///
/// confirm() runs on the thread of the connection's event loop; the prompt itself is asked on the GUI thread,
/// through a queued call to the context object, and the answer is carried back to the loop. Cancelling the
/// prompt, closing its window, or the context object going away before the prompt is shown all end in a
/// Decline: nobody answered Trust.
class AskpassPinPrompt final: public vthost::IPinPrompt
{
  public:
    /// Makes the id of each prompt asked; called on the connection's thread, and must not repeat one that the
    /// router still holds.
    using PromptIdSource = std::function<AskpassPromptId()>;

    /// @param router Finds the window's sink; shared.
    /// @param route The window the prompt belongs to.
    /// @param guiContext Lives on the GUI thread, and outlives every prompt it can still be asked for; the
    ///        prompt is asked there, never directly from the loop's thread. Borrowed.
    /// @param nextPromptId Where each prompt's id comes from.
    AskpassPinPrompt(std::shared_ptr<AskpassPromptRouter> router,
                     AskpassRouteId route,
                     QObject& guiContext,
                     PromptIdSource nextPromptId);

    /// @copydoc vthost::IPinPrompt::confirm
    [[nodiscard]] core::async::Task<vthost::PinDecision> confirm(std::string hostPort,
                                                                 std::string fingerprint) override;

  private:
    std::shared_ptr<AskpassPromptRouter> _router;
    AskpassRouteId _route;
    QObject& _guiContext;
    PromptIdSource _nextPromptId;
};

} // namespace contour::askpass
