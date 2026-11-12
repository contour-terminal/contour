// SPDX-License-Identifier: Apache-2.0
#include <contour/askpass/AskpassPinPrompt.hpp>

#include <core/async/Cancellation.hpp>
#include <core/async/ExecutorContext.hpp>
#include <core/async/StopToken.hpp>

#include <QtCore/QMetaObject>
#include <QtCore/QString>

#include <memory>
#include <utility>

#include <vthost/PinAnswer.hpp>

namespace contour::askpass
{

AskpassPinPrompt::AskpassPinPrompt(std::shared_ptr<AskpassPromptRouter> router,
                                   AskpassRouteId route,
                                   QObject& guiContext,
                                   PromptIdSource nextPromptId):
    _router { std::move(router) },
    _route { route },
    _guiContext { guiContext },
    _nextPromptId { std::move(nextPromptId) }
{
}

core::async::Task<vthost::PinDecision> AskpassPinPrompt::confirm(std::string hostPort,
                                                                 std::string fingerprint)
{
    // The synchronous part of a task on a loop: where the answer's waiter is resumed.
    auto* const executor = core::async::currentExecutor();
    if (executor == nullptr)
        co_return vthost::PinDecision::Decline;

    auto const answer = std::make_shared<vthost::PinAnswer>(*executor);
    auto const promptId = _nextPromptId();
    // The router is copied: the prompt object is the loop's, and may be gone by the time the GUI thread runs.
    // A temporary, so that the coroutine itself keeps no sender alive: only the queued call and the router
    // do.
    QMetaObject::invokeMethod(
        &_guiContext,
        [router = _router,
         route = _route,
         promptId,
         sender = vthost::PinAnswer::senderOf(answer),
         hostPort = QString::fromStdString(hostPort),
         fingerprint = QString::fromStdString(fingerprint)] {
            router->ask(route,
                        promptId,
                        AskpassPromptKind::PinConfirm,
                        hostPort,
                        fingerprint,
                        [sender](AskpassReply reply) {
                            sender->give(reply.outcome == AskpassOutcome::Answered
                                             ? vthost::PinDecision::Trust
                                             : vthost::PinDecision::Decline);
                        });
        },
        Qt::QueuedConnection);

    // A wait that is cancelled (the attach ran out of time, the window closed) takes its dialog down: an
    // answer nobody waits for must not be asked of the user. Queued behind the ask, so that it finds it.
    auto const stop = co_await core::async::thisCoroStopToken();
    auto const withdraw =
        core::async::StopCallback { stop, [router = _router, &context = _guiContext, promptId] {
                                       QMetaObject::invokeMethod(
                                           &context,
                                           [router, promptId] { router->withdraw(promptId); },
                                           Qt::QueuedConnection);
                                   } };
    co_return co_await answer->await();
}

} // namespace contour::askpass
