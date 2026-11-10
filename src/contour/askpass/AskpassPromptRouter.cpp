// SPDX-License-Identifier: Apache-2.0
#include <contour/askpass/AskpassPromptRouter.hpp>

#include <algorithm>
#include <ranges>
#include <utility>

namespace contour::askpass
{

std::optional<AskpassRouteId> pickAskpassSinkRoute(std::span<AskpassRouteId const> attached,
                                                   AskpassRouteId route)
{
    if (std::ranges::contains(attached, route))
        return route;
    // The windows in the order they came; the stand-in only when it is all there is.
    auto const real = std::ranges::find_if(attached, [](auto each) { return each != BootstrapRoute; });
    if (real != attached.end())
        return *real;
    if (!attached.empty())
        return attached.front();
    return std::nullopt;
}

std::optional<AskpassRouteId> AskpassPromptRouter::sinkRouteFor(AskpassRouteId route) const
{
    return pickAskpassSinkRoute(_attachOrder, route);
}

void AskpassPromptRouter::cancel(Prompt& prompt)
{
    if (!prompt.reply)
        return;
    auto reply = std::move(prompt.reply);
    reply(AskpassReply { .outcome = AskpassOutcome::Cancelled, .answer = {} });
}

void AskpassPromptRouter::deliver(Prompt prompt, AskpassRouteId sinkRoute)
{
    prompt.shownBy = sinkRoute;
    auto const id = prompt.id;
    auto const kind = prompt.kind;
    auto target = prompt.target;
    auto text = prompt.text;
    _shown.push_back(std::move(prompt));
    // The sink's reply ends the prompt here first, so that a reply after withdraw() or detach() is dropped.
    auto reply = [this, id](AskpassReply given) {
        auto const found = std::ranges::find(_shown, id, &Prompt::id);
        if (found == _shown.end())
        {
            wipe(given.answer);
            return;
        }
        auto original = std::move(found->reply);
        _shown.erase(found);
        if (original)
            original(std::move(given));
    };
    _sinks.at(sinkRoute)->ask(id, kind, std::move(target), std::move(text), std::move(reply));
}

void AskpassPromptRouter::attach(AskpassRouteId route, IAskpassPromptSink& sink)
{
    if (_isShutDown)
        return;
    if (!_sinks.contains(route))
        _attachOrder.push_back(route);
    _sinks[route] = &sink;
    // Prompts that waited for any window go to the one that came first, which is this one if it is alone.
    auto waiting = std::exchange(_waiting, {});
    for (auto& prompt: waiting)
    {
        auto const sinkRoute = sinkRouteFor(prompt.route);
        deliver(std::move(prompt), sinkRoute.value_or(route));
    }
    if (_onSinksChanged)
        _onSinksChanged();
}

void AskpassPromptRouter::detach(AskpassRouteId route)
{
    if (_sinks.erase(route) == 0)
        return;
    std::erase(_attachOrder, route);
    auto ended = std::vector<Prompt> {};
    std::erase_if(_shown, [&](Prompt& prompt) {
        if (prompt.shownBy != route)
            return false;
        ended.push_back(std::move(prompt));
        return true;
    });
    for (auto& prompt: ended)
        cancel(prompt);
    if (_onSinksChanged)
        _onSinksChanged();
}

void AskpassPromptRouter::ask(AskpassRouteId route,
                              AskpassPromptId promptId,
                              AskpassPromptKind kind,
                              QString target,
                              QString prompt,
                              std::function<void(AskpassReply)> reply)
{
    auto asked = Prompt { .id = promptId,
                          .route = route,
                          .kind = kind,
                          .target = std::move(target),
                          .text = std::move(prompt),
                          .reply = std::move(reply),
                          .shownBy = AnyWindow };
    if (_isShutDown)
    {
        cancel(asked);
        return;
    }
    if (auto const sinkRoute = sinkRouteFor(route))
        deliver(std::move(asked), *sinkRoute);
    else
    {
        _waiting.push_back(std::move(asked));
        if (_onWaiting)
            _onWaiting();
    }
}

void AskpassPromptRouter::withdraw(AskpassPromptId promptId)
{
    if (auto const waiting = std::ranges::find(_waiting, promptId, &Prompt::id); waiting != _waiting.end())
    {
        _waiting.erase(waiting);
        return;
    }
    auto const shown = std::ranges::find(_shown, promptId, &Prompt::id);
    if (shown == _shown.end())
        return;
    auto const sinkRoute = shown->shownBy;
    _shown.erase(shown);
    if (auto const sink = _sinks.find(sinkRoute); sink != _sinks.end())
        sink->second->withdraw(promptId);
}

void AskpassPromptRouter::shutdown()
{
    _isShutDown = true;
    _sinks.clear();
    _attachOrder.clear();
    auto shown = std::exchange(_shown, {});
    auto waiting = std::exchange(_waiting, {});
    for (auto& prompt: shown)
        cancel(prompt);
    for (auto& prompt: waiting)
        cancel(prompt);
    if (_onSinksChanged)
        _onSinksChanged();
}

} // namespace contour::askpass
