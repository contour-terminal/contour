// SPDX-License-Identifier: Apache-2.0
#include <vthost/tmux/TmuxGateway.h>

#include <chrono>
#include <format>
#include <utility>
#include <variant>

#include <net/AsyncBufferedReader.h>

namespace vthost::tmux
{

using namespace std::chrono_literals;

TmuxGateway::TmuxGateway(net::EventLoop& loop,
                         std::unique_ptr<net::ISocket> connection,
                         GatewayEvents& events):
    _connection(std::move(connection)),
    _writer(loop, _connection.get(), std::size_t { 256 } * 1024),
    _events(events)
{
}

void TmuxGateway::sendCommand(std::string command, CommandCallback callback)
{
    command += '\n';
    if (!_writer.enqueue(std::move(command)))
    {
        // The queue's overflow contract: a dropped command means its callback
        // (not yet queued) must be failed so the caller can clean up, rather
        // than left dangling in _pending to desync every later reply.
        if (callback)
            callback(false, {});
        _writer.close();
        _connection->close();
        return;
    }
    _pending.push_back(std::move(callback));
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

void TmuxGateway::sendRawInput(uint64_t pane, std::string_view bytes)
{
    // Hex form (-H): every byte survives verbatim — control bytes, ESC, CR —
    // with no quoting rules in the way. This is the GUI input path, where the
    // InputGenerator's encodings are arbitrary bytes, not typed text.
    constexpr std::size_t BatchLimit = 256;
    while (!bytes.empty())
    {
        auto const batch = bytes.substr(0, BatchLimit);
        bytes.remove_prefix(batch.size());

        auto command = std::format("send-keys -t %{} -H", pane);
        command.reserve(command.size() + (batch.size() * 3));
        for (auto const ch: batch)
            std::format_to(std::back_inserter(command), " {:02x}", static_cast<uint8_t>(ch));
        sendCommand(std::move(command));
    }
}

void TmuxGateway::detach()
{
    _detached = true;
    // An empty line detaches (control.c:547); if it cannot even be queued,
    // fail every pending callback so nothing leaks, then sever the transport.
    if (!_writer.enqueue("\n"))
    {
        for (auto& cb: _pending)
        {
            if (cb)
                cb(false, {});
        }
        _pending.clear();
        _writer.close();
        _connection->close();
    }
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
            if (auto const* begin = std::get_if<GuardBegin>(&event))
            {
                _state = State::InGuard;
                _guardNumber = begin->number;
                _openingGuard = true;
                _guardBody.clear();
            }
            else if (std::holds_alternative<ExitEvent>(event))
                dispatchNotification(event);
            return;

        case State::Idle:
            if (auto const* begin = std::get_if<GuardBegin>(&event))
            {
                _state = State::InGuard;
                _guardNumber = begin->number;
                _guardBody.clear();
                return;
            }
            dispatchNotification(event);
            return;

        case State::InGuard:
            // The guard BODY is transmitted RAW: capture-pane content can contain
            // a line that reads exactly like "%end N F". Match the command number
            // (as real control clients do) so only the genuine terminator closes
            // the block — an embedded "%end" with a different (or no) number is
            // body, not the end, and mis-closing here desyncs every later reply.
            if (auto const* end = std::get_if<GuardEnd>(&event);
                end != nullptr && end->number == _guardNumber)
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
        // Burst boundary: once nothing more arrived together, the batch of
        // notifications for one server operation is fully applied, so the
        // consumer may settle deferred verdicts (a pane parked by a move's
        // source %layout-change has by now been reclaimed by its destination).
        //
        // Gate on the whole buffer being empty, not merely on there being no
        // COMPLETE line: a partial (unterminated) trailing line is a mid-burst read
        // split, and draining then would reconcile before the rest of that line
        // (e.g. a pane move's destination %layout-change) has been parsed.
        if (reader.buffered() == 0)
            _events.notificationsDrained();
    }

    // Disconnect or exit: fail every orphaned callback so nothing leaks.
    for (auto& cb: _pending)
    {
        if (cb)
            cb(false, {});
    }
    _pending.clear();

    co_await _writer.flushThenClose();
    _connection->close();
}

} // namespace vthost::tmux
