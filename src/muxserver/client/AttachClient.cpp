// SPDX-License-Identifier: Apache-2.0
#include <muxserver/client/AttachClient.h>

#include <libunicode/convert.h>

#include <chrono>
#include <iterator>
#include <utility>
#include <variant>
#include <vector>

#include <muxserver/PduPump.h>
#include <net/Sockets.h>

namespace muxserver::client
{

using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// RemoteScreen

void RemoteScreen::apply(proto::SessionState const& state)
{
    session = state.session;
    columns = state.columns;
    lines = state.lines;
    screenType = state.screenType;
    cursorLine = state.cursorLine;
    cursorColumn = state.cursorColumn;
    title = state.title;
}

void RemoteScreen::apply(proto::Delta const& delta)
{
    session = delta.session;
    if (delta.snapshot != 0)
        rows.clear(); // a snapshot replaces everything the client held

    generation = delta.generation;
    seqno = delta.seqno;
    viewportBase = delta.stableViewportBase;
    cursorLine = delta.cursorLine;
    cursorColumn = delta.cursorColumn;
    setModes = delta.setModes;

    for (auto const& line: delta.lines)
        rows.insert_or_assign(line.stableId, line);
    for (auto const& entry: delta.hyperlinks)
        hyperlinks.insert_or_assign(entry.id, entry.uri);

    // Trim client-side scrollback: everything HistoryKeep above the viewport.
    rows.erase(rows.begin(), rows.lower_bound(viewportBase - HistoryKeep));
}

proto::WireLine const* RemoteScreen::rowAt(int32_t line) const
{
    auto const it = rows.find(viewportBase + line);
    return it != rows.end() ? &it->second : nullptr;
}

std::string RemoteScreen::viewportText() const
{
    auto text = std::string {};
    for (auto line = int32_t { 0 }; std::cmp_less(line, lines); ++line)
    {
        auto rendered = std::string {};
        if (auto const* row = rowAt(line))
            for (auto const& cell: row->cells)
            {
                if (cell.codepoint == 0)
                {
                    rendered += ' ';
                    continue;
                }
                unicode::convert_to<char>(std::u32string_view(&cell.codepoint, 1),
                                          std::back_inserter(rendered));
                for (auto const extra: cell.clusterExtras)
                    unicode::convert_to<char>(std::u32string_view(&extra, 1), std::back_inserter(rendered));
            }
        while (!rendered.empty() && rendered.back() == ' ')
            rendered.pop_back();
        text += rendered;
        text += '\n';
    }
    return text;
}

// ---------------------------------------------------------------------------
// AttachClient

AttachClient::AttachClient(net::EventLoop& loop, std::unique_ptr<net::ISocket> connection):
    _connection(std::move(connection)),
    _writer(loop, _connection.get(), std::size_t { 1 } * 1024 * 1024)
{
}

void AttachClient::send(proto::DecodedPdu const& pdu)
{
    auto sink = proto::Writer {};
    proto::encodePdu(sink, _nextSerial++, pdu);
    auto const bytes = sink.view();
    if (!_writer.enqueue(std::string { reinterpret_cast<char const*>(bytes.data()), bytes.size() }))
    {
        // The queue's overflow contract: dropping a frame mid-stream (a
        // keystroke, a resize) silently desyncs the daemon — sever instead.
        _writer.close();
        _connection->close();
    }
}

void AttachClient::sendInput(uint64_t session, std::string_view bytes)
{
    auto input = proto::Input { .session = session, .data = {} };
    input.data.reserve(bytes.size());
    for (auto const ch: bytes)
        input.data.push_back(static_cast<std::byte>(ch));
    send(proto::DecodedPdu { input });
}

void AttachClient::requestResize(uint32_t columns, uint32_t lines)
{
    send(proto::DecodedPdu { proto::ResizeRequest { .columns = columns, .lines = lines } });
}

void AttachClient::fetchImage(uint32_t imageId)
{
    send(proto::DecodedPdu { proto::FetchImage { .imageId = imageId } });
}

void AttachClient::detach()
{
    _detached = true;
    _writer.close();
    _connection->close();
}

void AttachClient::handlePdu(proto::DecodedPdu const& pdu)
{
    if (auto const* hello = std::get_if<proto::ServerHello>(&pdu))
    {
        if (hello->codecVersion == proto::CodecVersion)
            _connected = true;
        else
            _versionMismatch = true;
        return;
    }
    if (auto const* state = std::get_if<proto::SessionState>(&pdu))
    {
        _screens[state->session].apply(*state);
        return;
    }
    if (auto const* delta = std::get_if<proto::Delta>(&pdu))
    {
        auto& screen = _screens[delta->session];
        screen.apply(*delta);
        if (_onUpdate)
            _onUpdate(screen, *delta);
        return;
    }
    // ImageData/ImageGone are consumed by the frontend's update handler in a
    // later slice; unknown PDUs are ignored for forward compatibility.
}

coro::Task<void> AttachClient::run()
{
    send(proto::DecodedPdu { proto::ClientHello {} });

    co_await pumpPdus(_connection.get(), [this](proto::DecodedFrame const& frame) {
        handlePdu(frame.pdu);
        return !_detached && !_versionMismatch;
    });

    if (!_detached)
    {
        co_await _writer.flushThenClose();
        _connection->close();
    }
}

} // namespace muxserver::client
