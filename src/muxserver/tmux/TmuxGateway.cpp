// SPDX-License-Identifier: Apache-2.0
#include <muxserver/tmux/TmuxGateway.h>

#include <chrono>
#include <format>
#include <utility>
#include <variant>

#include <net/AsyncBufferedReader.h>

namespace muxserver::tmux
{

using namespace std::chrono_literals;

TmuxGateway::TmuxGateway(net::EventLoop& loop,
                         std::unique_ptr<net::ISocket> connection,
                         GatewayEvents& events):
    _loop(loop),
    _connection(std::move(connection)),
    _writer(loop, _connection.get(), std::size_t { 256 } * 1024),
    _events(events)
{
}

void TmuxGateway::sendCommand(std::string command, CommandCallback callback)
{
    _pending.push_back(std::move(callback));
    command += '\n';
    std::ignore = _writer.enqueue(std::move(command));
}

void TmuxGateway::sendKeys(uint64_t pane, std::string_view text)
{
    // iTerm2's batching: at most 1000 characters per send-keys command, sent
    // literally (-l) with single quotes closed around embedded quotes.
    constexpr std::size_t BatchLimit = 1000;
    while (!text.empty())
    {
        auto const batch = text.substr(0, BatchLimit);
        text.remove_prefix(batch.size());

        // Double-quote quoting with backslash escapes: the dialect both real
        // tmux's argument parser and this project's splitCommandLine accept.
        auto quoted = std::string { "\"" };
        for (auto const ch: batch)
        {
            if (ch == '"' || ch == '\\')
                quoted += '\\';
            quoted += ch;
        }
        quoted += '"';
        sendCommand(std::format("send-keys -t %{} -l -- {}", pane, quoted));
    }
}

void TmuxGateway::detach()
{
    _detached = true;
    std::ignore = _writer.enqueue("\n"); // an empty line detaches (control.c:547)
}

void TmuxGateway::dispatchNotification(ControlEvent const& event)
{
    if (auto const* output = std::get_if<OutputEvent>(&event))
        _events.outputReceived(output->pane, output->bytes);
    else if (auto const* layout = std::get_if<LayoutChangeEvent>(&event))
        _events.layoutChanged(layout->window, layout->layout);
    else if (auto const* added = std::get_if<WindowAddEvent>(&event))
        _events.windowAdded(added->window);
    else if (auto const* closed = std::get_if<WindowCloseEvent>(&event))
        _events.windowClosed(closed->window);
    else if (auto const* renamed = std::get_if<WindowRenamedEvent>(&event))
        _events.windowRenamed(renamed->window, renamed->name);
    else if (auto const* session = std::get_if<SessionChangedEvent>(&event))
        _events.sessionChanged(session->session, session->name);
    else if (auto const* pause = std::get_if<PauseEvent>(&event))
        _events.panePaused(pause->pane, pause->paused);
    else if (auto const* exit = std::get_if<ExitEvent>(&event))
    {
        _exited = true;
        _events.exited(exit->reason);
    }
    // UnknownNotification: tolerated, deliberately dropped.
}

void TmuxGateway::handleLine(std::string_view line)
{
    auto const event = classifyLine(line);

    switch (_state)
    {
        case State::Recovery:
            // iTerm2's recovery rule: discard everything until the opening
            // %begin (or an immediate %exit) — the server may have queued
            // stale output before the control client was recognized.
            if (std::holds_alternative<GuardBegin>(event))
            {
                _state = State::InGuard;
                _openingGuard = true;
                _guardBody.clear();
            }
            else if (std::holds_alternative<ExitEvent>(event))
                dispatchNotification(event);
            return;

        case State::Idle:
            if (std::holds_alternative<GuardBegin>(event))
            {
                _state = State::InGuard;
                _guardBody.clear();
                _guardIsError = false;
                return;
            }
            dispatchNotification(event);
            return;

        case State::InGuard:
            if (auto const* end = std::get_if<GuardEnd>(&event))
            {
                _state = State::Idle;
                if (_openingGuard)
                {
                    // The implicit initial command's guard: completing it
                    // un-gates notification handling (iTerm2's rule).
                    _openingGuard = false;
                    _initialised = true;
                }
                else if (!_pending.empty())
                {
                    auto callback = std::move(_pending.front());
                    _pending.pop_front();
                    if (callback)
                        callback(!end->isError, _guardBody);
                }
                _guardBody.clear();
                return;
            }
            // tmux's dual-queue ordering guarantees no notification lands
            // inside a guard block: everything here is response body — even
            // lines starting with '%' (capture-pane content may).
            if (auto const* body = std::get_if<BodyLine>(&event))
                _guardBody.push_back(body->text);
            else
                _guardBody.emplace_back(line);
            return;
    }
}

coro::Task<void> TmuxGateway::run()
{
    auto reader = net::AsyncBufferedReader { _connection.get() };
    while (!_exited)
    {
        auto line = co_await reader.readLine();
        if (!line.has_value())
            break; // disconnect
        handleLine(*line);
    }

    while (_writer.queuedBytes() > 0 || _writer.draining())
        co_await _loop.delay(1ms);
    _writer.close();
    _connection->close();
}

} // namespace muxserver::tmux
