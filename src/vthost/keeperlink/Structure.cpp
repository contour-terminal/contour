// SPDX-License-Identifier: Apache-2.0
#include <vthost/keeperlink/Structure.hpp>

#include <core/codec/Stream.hpp>
#include <core/net/EventLoop.hpp>

#include <algorithm>
#include <utility>

#include <vthost/Logging.hpp>
#include <vthost/handover/HandoverCapture.hpp>
#include <vthost/handover/HandoverCodec.hpp>
#include <vthost/keeper/Messages.hpp>
#include <vthost/keeperlink/KeeperLink.hpp>

namespace vthost::keeperlink
{

std::string_view describe(StructureError error) noexcept
{
    switch (error)
    {
        case StructureError::BadMagic: return "it is no structure";
        case StructureError::NewerVersion: return "a newer release wrote it";
        case StructureError::Malformed: return "it does not decode";
    }
    return "it cannot be read";
}

handover::HandoverImage captureStructure(SessionHost const& host, StructureExtras const& extras)
{
    auto structure = handover::hostStructureOf(host);
    structure.daemonInstance = extras.daemonInstance;
    structure.fromRelease = extras.release;
    structure.upgradeGeneration = extras.upgradeGeneration;
    structure.listeners = extras.listeners;
    // Nothing that names a descriptor: the keeper holds those, and hands them back beside the structure.
    for (auto& listener: structure.listeners)
        listener.handle = {};
    return structure;
}

std::vector<uint8_t> encodeStructure(handover::HandoverImage const& structure)
{
    auto writer = core::codec::Writer {};
    writer.raw(StructureMagic);
    writer.varint(StructureVersion);
    writer.bytes(handover::encodeHandover(structure));
    return std::move(writer).take();
}

std::expected<handover::HandoverImage, StructureError> decodeStructure(std::span<uint8_t const> blob)
{
    auto reader = core::codec::Reader { blob };
    auto const magic = reader.raw(StructureMagic.size());
    if (!magic || !std::ranges::equal(*magic, StructureMagic))
        return std::unexpected { StructureError::BadMagic };
    auto const version = reader.varint();
    if (!version || *version == 0)
        return std::unexpected { StructureError::Malformed };
    if (*version > StructureVersion)
        return std::unexpected { StructureError::NewerVersion };
    auto const body = reader.bytes();
    if (!body || !reader.atEnd())
        return std::unexpected { StructureError::Malformed };
    return handover::decodeHandover(*body).transform_error(
        [](handover::HandoverError) { return StructureError::Malformed; });
}

// {{{ KeeperStructureSink

KeeperStructureSink::KeeperStructureSink(KeeperLink& link, handover::HandoverMemory& memory):
    _link { link }, _memory { memory }
{
}

void KeeperStructureSink::push(std::span<uint8_t const> blob)
{
    auto const object = _memory.write(blob);
    if (!object)
    {
        // The keeper keeps the structure it has: a restart after a crash serves the one before this change.
        errorLog()("the structure could not be written for the keeper (error {}); it keeps the last one",
                   std::to_underlying(object.error()));
        return;
    }
    _link.post(keeper::Structure {}, { object->native() });
    _memory.discard(*object);
}

// }}}
// {{{ StructurePusher

StructurePusher::StructurePusher(core::net::EventLoop& loop,
                                 SessionHost& host,
                                 StructureSink& sink,
                                 StructureExtras extras,
                                 std::chrono::milliseconds debounce):
    _loop { loop }, _host { host }, _sink { sink }, _extras { std::move(extras) }, _debounce { debounce }
{
    _host.attachStructureObserver(this);
    push();
}

StructurePusher::~StructurePusher()
{
    _host.attachStructureObserver(nullptr);
}

void StructurePusher::note(StructureEvent event)
{
    switch (pushTimingOf(event))
    {
        case PushTiming::Synchronous: pushBeforeAnnouncement(); return;
        case PushTiming::Debounced: break;
    }
    // Within the debounce of the first change not pushed yet: a drag that goes on is pushed once a period,
    // not never.
    if (_pending && !_pending->settled())
        return;
    _pending.emplace(_loop, _loop.clock().now() + _debounce, &pushDebounced, this);
}

void StructurePusher::pushBeforeAnnouncement()
{
    if (_coalesced == Coalesced::Owed)
        return; // the flow spawned for this turn's first change, ahead of any announcement, pushes it too
    // Something queued ahead of the flow would run first, and may be a connection's drain.
    if (_loop.readyCount() != 0)
    {
        push();
        return;
    }
    _coalesced = Coalesced::Owed;
    _loop.spawn(pushOwed(_self));
}

core::async::Task<void> StructurePusher::pushOwed(std::weak_ptr<StructurePusher*> pusher)
{
    if (auto const self = pusher.lock(); self && (*self)->_coalesced == Coalesced::Owed)
        (*self)->push();
    co_return;
}

void StructurePusher::push()
{
    // What a pending debounced or coalesced push would have carried is in this one.
    if (_pending)
        _pending->disarm();
    _coalesced = Coalesced::None;
    _sink.push(encodeStructure(captureStructure(_host, _extras)));
}

void StructurePusher::pushDebounced(void* pusher)
{
    static_cast<StructurePusher*>(pusher)->push();
}

// }}}

} // namespace vthost::keeperlink
