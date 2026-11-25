// SPDX-License-Identifier: Apache-2.0
//
// contour-extension-host-hostile: the extension host, misbehaving as `--hostile=<name>` says
// (HostileModes.hpp), and otherwise the real one. Started by the crash and hostility tests only; never
// installed.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <contour-extension-host/testing/HostileModes.hpp>
#include <vtextension/wasm/host/HelperEngine.hpp>
#include <vtextension/wasm/host/HostProcess.hpp>
#include <vtextension/wasm/host/Sandbox.hpp>
#include <vtextension/wasm/remote/proto/Codec.hpp>
#include <vtextension/wasm/remote/proto/Pdu.hpp>
#include <vtextension/wasm/remote/proto/Wire.hpp>
#include <vtextension/wasm/remote/transport/Channel.hpp>
#include <vtextension/wasm/remote/transport/HelperArguments.hpp>
#include <vtextension/wasm/remote/transport/OwnedHandle.hpp>
#include <vtextension/wasm/remote/transport/SharedObject.hpp>
#include <vtextension/wasm/remote/transport/WakeSignal.hpp>
#include <vtextension/wasm/remote/transport/testing/HandleCopies.hpp>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <Windows.h>
#else
    #include <sys/resource.h>

    #include <fcntl.h>
#endif

namespace proto = vtextension::wasm::remote::proto;
namespace transport = vtextension::wasm::remote::transport;
using contour::extension_host::testing::AfterContourEnds;
using contour::extension_host::testing::GarbageAnswer;
using contour::extension_host::testing::GrantedMemory;
using contour::extension_host::testing::HostileAction;
using contour::extension_host::testing::HostileIncident;
using contour::extension_host::testing::hostileModeNamed;
using contour::extension_host::testing::HostileModeRow;
using contour::extension_host::testing::HostileOption;
using contour::extension_host::testing::HostMemory;
using contour::extension_host::testing::WakeSource;

namespace
{

/// The exit code of a hostile host whose own command line is wrong: none of the real host's codes (0, 70 and
/// up, HostProcess.hpp), nor a sanitizer's.
constexpr auto HostileSetupFailed = 79;

/// The exit code of a host that ends in the middle of serving (HostileAction::ForwardThenExit).
constexpr auto AbruptExitCode = 3;

/// The limits both ends apply.
constexpr auto Limits = proto::PduLimits {};

/// Ends the hostile host before it served, saying why.
[[nodiscard]] int refuse(char const* reason)
{
    std::println(stderr, "contour-extension-host-hostile: {}", reason);
    return HostileSetupFailed;
}

/// @return What makes the real host's engine, reaching memory as @p memory says.
[[nodiscard]] vtextension::wasm::host::EngineFactory enginesOf(HostMemory memory)
{
    switch (memory)
    {
        case HostMemory::AsBuilt: return &vtextension::wasm::host::makeHelperEngine;
        case HostMemory::Copied: return &vtextension::wasm::host::makeCopyingHelperEngine;
    }
    return &vtextension::wasm::host::makeHelperEngine;
}

/// Runs the real host on @p arguments, its engine reaching memory as @p memory says.
[[nodiscard]] int serve(std::vector<std::string> const& arguments, HostMemory memory = HostMemory::AsBuilt)
{
    auto views = std::vector<std::string_view> {};
    for (auto const& argument: arguments)
        views.emplace_back(argument);
    // Hostility is measured against a host that confines nothing more than its hardening: a sandbox would
    // refuse what the hostile modes do to the host itself.
    auto confinement = vtextension::wasm::host::systemConfinement();
    confinement.sandbox =
        [](vtextension::wasm::host::SandboxDescriptors,
           vtextension::wasm::host::GuestCode) -> std::unique_ptr<vtextension::wasm::host::ISandbox> {
        return std::make_unique<vtextension::wasm::host::NullSandbox>();
    };
    return vtextension::wasm::host::runExtensionHost(views, enginesOf(memory), confinement);
}

/// Has the crash that follows end the process at once: without a core dump, which a system that collects
/// them (systemd-coredump) writes before the process is gone -- long enough for Contour to stop it first, and
/// count the crash as a host it stopped rather than one that died on its own. Sanitizer runtimes disable
/// core dumps themselves, so only plain builds need this.
void withoutCoreDump() noexcept
{
#ifndef _WIN32
    auto const none = rlimit { .rlim_cur = 0, .rlim_max = 0 };
    std::ignore = ::setrlimit(RLIMIT_CORE, &none);
#endif
}

/// Crashes as std::abort() does, with no dialog on Windows.
[[noreturn]] void crashByAbort()
{
    withoutCoreDump();
#ifdef _WIN32
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    std::abort();
}

/// Dies as a segmentation fault does, past any handler a sanitizer runtime installed.
[[noreturn]] void crashBySegfault()
{
    withoutCoreDump();
#ifdef _WIN32
    ::TerminateProcess(::GetCurrentProcess(), static_cast<UINT>(STATUS_ACCESS_VIOLATION));
#else
    std::signal(SIGSEGV, SIG_DFL);
    std::raise(SIGSEGV);
#endif
    std::abort(); // Not reached.
}

/// @return The kind of the frame whose header @p buffer starts with; frameSizeOf() accepted it.
[[nodiscard]] proto::PduKind kindAtFront(std::span<uint8_t const> buffer)
{
    auto header = proto::Reader { buffer };
    std::ignore = header.varint64();
    return static_cast<proto::PduKind>(header.varint32().value_or(0));
}

/// @return How many bytes of @p frame its length and kind span.
[[nodiscard]] std::size_t headerBytesOf(std::span<uint8_t const> frame)
{
    auto header = proto::Reader { frame };
    std::ignore = header.varint64();
    std::ignore = header.varint32();
    return header.consumed();
}

/// @return A MemoryData frame of @p size bytes, all of them zero.
[[nodiscard]] std::vector<uint8_t> memoryDataOf(std::size_t size)
{
    return proto::encodeFrame(
               proto::MemoryData { .status = proto::MemoryStatus::Ok, .bytes = std::vector<std::byte>(size) })
        .value_or(std::vector<uint8_t> {});
}

/// One channel Contour handed over, and the one the real host serves in its place.
struct RelayedChannel
{
    proto::ChannelKind kind;    ///< Control or lane.
    transport::Channel contour; ///< Contour's end, as this process inherited it.
    transport::Channel host;    ///< This process's end of the channel the real host serves.
    std::mutex contourSends;    ///< Serializes what the host's frames and the relay's own answers send.
};

/// What becomes of a frame once a pump read its header.
enum class Verdict : uint8_t
{
    Forward, ///< It is passed on, as every frame before it.
    Discard, ///< It is read, and passed on to nobody.
    Stop,    ///< Nothing more is read this way: not the rest of it, nor anything after.
    Hold,    ///< It and everything after it this way are read, and passed on to nobody.
};

/// @return A Grow decoded from the frame of @p size bytes that @p pending starts with, rewritten by
///         @p rewrite, as a frame; nothing if it is no Grow.
[[nodiscard]] std::optional<std::vector<uint8_t>> rewrittenGrow(
    std::span<uint8_t const> frame,
    proto::ChannelKind channel,
    std::function<void(proto::Grow&)> const& rewrite)
{
    auto decoded = proto::decodeFrame(frame, proto::Direction::ToParent, channel, Limits);
    auto* const grow = decoded ? std::get_if<proto::Grow>(&decoded->pdu) : nullptr;
    if (grow == nullptr)
        return std::nullopt;
    rewrite(*grow);
    auto encoded = proto::encodeFrame(*grow);
    return encoded ? std::optional { *std::move(encoded) } : std::nullopt;
}

/// The memory of the latest instance Contour granted, which one thread scribbles on: one at a time, so that a
/// host making many instances does not run out of address space for its scribbling. The thread starts with
/// the scribbler, before the host caps how many threads it may start.
class Scribbler
{
  public:
    Scribbler(): _thread { [this](std::stop_token const& stop) { run(stop); } } {}
    Scribbler(Scribbler const&) = delete;
    Scribbler(Scribbler&&) = delete;
    Scribbler& operator=(Scribbler const&) = delete;
    Scribbler& operator=(Scribbler&&) = delete;
    ~Scribbler() = default;

    /// Scribbles on the first @p opened bytes of @p mapping from now on, in place of what it scribbled on
    /// before.
    void scribbleOn(transport::SharedMapping mapping, uint64_t opened)
    {
        auto const lock = std::scoped_lock { _mutex };
        _mapping = std::move(mapping);
        _opened = opened;
    }

  private:
    /// Flips bytes of the current memory, at places a fixed sequence picks, until @p stop is requested.
    void run(std::stop_token const& stop)
    {
        auto state = uint32_t { 0x5eed };
        while (!stop.stop_requested())
        {
            {
                auto const lock = std::scoped_lock { _mutex };
                auto const bytes = _mapping ? _mapping->bytes().first(_opened) : std::span<std::byte> {};
                for ([[maybe_unused]] auto const flip: std::views::iota(0, bytes.empty() ? 0 : 4096))
                {
                    state = (state * 1'664'525U) + 1'013'904'223U;
                    bytes[state % bytes.size()] ^= std::byte { 0xFF };
                }
            }
            std::this_thread::yield();
        }
    }

    std::mutex _mutex;                                ///< Guards what follows.
    std::optional<transport::SharedMapping> _mapping; ///< The memory scribbled on; nothing before the first.
    uint64_t _opened = 0;                             ///< How many of its bytes are scribbled on.
    std::jthread _thread;                             // Last, so that it is joined before the rest goes.
};

/// Relays every channel between Contour's end and the real host's, both ways, frame by frame -- until the
/// incident's trigger, once, where its action happens. The descriptor Contour grants with an Instantiate is
/// forwarded with it, and the memory it names is scribbled on if the row says so.
class Relay
{
  public:
    /// @param incident What happens, and where; nothing if nothing does.
    /// @param granted What becomes of the memory Contour grants.
    /// @param channels Every channel relayed; the first is the control channel.
    Relay(std::optional<HostileIncident> incident,
          GrantedMemory granted,
          std::vector<std::unique_ptr<RelayedChannel>> channels):
        _incident { incident }, _granted { granted }, _channels { std::move(channels) }
    {
        // Every thread before the host caps how many it may start.
        if (_granted == GrantedMemory::Scribbled)
            _scribbler.emplace();
        for (auto const& channel: _channels)
            for (auto const direction: { proto::Direction::ToHelper, proto::Direction::ToParent })
                _pumps.emplace_back(
                    [this, channel = channel.get(), direction] { pump(*channel, direction); });
    }
    Relay(Relay const&) = delete;
    Relay(Relay&&) = delete;
    Relay& operator=(Relay const&) = delete;
    Relay& operator=(Relay&&) = delete;
    ~Relay() = default;

  private:
    /// Whether the incident happened yet.
    enum class Progress : uint8_t
    {
        Pending,
        Happened,
    };

    /// Whether Contour's channels are still read.
    enum class ContourReads : uint8_t
    {
        Open,
        Stopped,
    };

    /// One way of one channel: where a pump reads, and where it writes.
    struct Way
    {
        RelayedChannel& channel;
        proto::Direction direction;

        [[nodiscard]] transport::Channel& source() const noexcept
        {
            return direction == proto::Direction::ToHelper ? channel.contour : channel.host;
        }

        [[nodiscard]] transport::Channel& target() const noexcept
        {
            return direction == proto::Direction::ToHelper ? channel.host : channel.contour;
        }

        /// Sends @p bytes to @p end, which is one of this channel's.
        [[nodiscard]] bool sendTo(transport::Channel& end, std::span<uint8_t const> bytes) const
        {
            if (&end != &channel.contour)
                return end.send(bytes).has_value();
            auto const lock = std::scoped_lock { channel.contourSends };
            return end.send(bytes).has_value();
        }
    };

    /// @return Whether the frame of @p kind that @p way carries is where the incident happens: the first
    ///         that matches its trigger.
    [[nodiscard]] bool isIncident(Way const& way, proto::PduKind kind)
    {
        if (!_incident)
            return false;
        auto const& trigger = _incident->trigger;
        if (way.direction != trigger.direction || way.channel.kind != trigger.channel || kind != trigger.kind)
            return false;
        if (trigger.after && (_passed.load() & bitOf(*trigger.after)) == 0)
            return false;
        return _progress.exchange(Progress::Happened) == Progress::Pending;
    }

    [[nodiscard]] static uint32_t bitOf(proto::PduKind kind) noexcept
    {
        return uint32_t { 1 } << std::to_underlying(kind);
    }

    /// @return Whether @p way is where Contour grants descriptors: its control channel, towards the host.
    [[nodiscard]] static bool isGranting(Way const& way) noexcept
    {
        return way.channel.kind == proto::ChannelKind::Control && way.direction == proto::Direction::ToHelper;
    }

    /// Receives some bytes on @p way into @p chunk. A descriptor that comes with them on the way Contour
    /// grants them is kept, for the Instantiate it came with.
    /// @return How many arrived; nothing once the way ended.
    [[nodiscard]] std::optional<std::size_t> receiveOn(Way const& way, std::span<uint8_t> chunk)
    {
#ifndef _WIN32
        if (isGranting(way))
        {
            auto arrived = std::vector<transport::OwnedHandle> {};
            auto const received = way.source().receiveSomeWithHandles(chunk, arrived);
            if (!received)
                return std::nullopt;
            for (auto& handle: arrived)
                _grantedHandles.push_back(std::move(handle));
            return *received;
        }
#endif
        auto const received = way.source().receiveSome(chunk);
        return received ? std::optional { *received } : std::nullopt;
    }

    /// Receives on @p way until @p pending holds @p size bytes.
    /// @return Whether they arrived.
    [[nodiscard]] bool complete(Way const& way, std::vector<uint8_t>& pending, std::size_t size)
    {
        auto chunk = std::array<uint8_t, 4096> {};
        while (pending.size() < size)
        {
            auto const received = receiveOn(way, chunk);
            if (!received)
                return false;
            pending.insert(
                pending.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(*received));
        }
        return true;
    }

    /// Forwards the Instantiate whose frame, @p size bytes, @p pending starts with, with the descriptor
    /// Contour granted with it (POSIX); notes its object's size, and scribbles on the object if the row says
    /// so.
    /// @return What becomes of the frame.
    [[nodiscard]] Verdict relayInstantiate(Way const& way, std::vector<uint8_t>& pending, std::size_t size)
    {
        if (!complete(way, pending, size))
            return Verdict::Stop;
        auto const frame = std::span<uint8_t const> { pending }.first(size);
        auto const decoded = proto::decodeFrame(frame, way.direction, way.channel.kind, Limits);
        auto const* const instantiate = decoded ? std::get_if<proto::Instantiate>(&decoded->pdu) : nullptr;
        if (instantiate == nullptr || instantiate->memory.objectBytes == 0)
            return Verdict::Forward;
        auto const& grant = instantiate->memory;
        _objectBytes.store(grant.objectBytes);
#ifdef _WIN32
        // The handle's value is this process's: the real host serves in it.
        auto const object = static_cast<transport::NativeHandle>(reinterpret_cast<void*>(grant.handle));
        if (_granted == GrantedMemory::Scribbled)
            scribbleOn(object, grant);
        return Verdict::Forward;
#else
        if (_grantedHandles.empty())
            return Verdict::Forward;
        auto const object = std::move(_grantedHandles.front());
        _grantedHandles.pop_front();
        if (_granted == GrantedMemory::Scribbled)
            scribbleOn(object.get(), grant);
        return way.target().sendWithHandle(frame, object.get()) ? Verdict::Discard : Verdict::Stop;
#endif
    }

    /// Maps @p object as @p grant describes it, and has the bytes the memory spans at first scribbled on
    /// until the next grant, or the relay goes.
    void scribbleOn(transport::NativeHandle object, proto::MemoryGrant const& grant)
    {
        auto mapping =
            transport::SharedMapping::map(object, grant.objectBytes, transport::MapAccess::ReadWrite);
        if (!mapping || grant.initialBytes == 0 || !mapping->commit(0, grant.initialBytes))
            return;
        _scribbler->scribbleOn(*std::move(mapping), grant.initialBytes);
    }

    /// Forwards the Grow whose frame, @p size bytes, @p pending starts with, rewritten by @p rewrite.
    /// @return What becomes of the frame.
    [[nodiscard]] Verdict forwardGrow(Way const& way,
                                      std::vector<uint8_t>& pending,
                                      std::size_t size,
                                      std::function<void(proto::Grow&)> const& rewrite)
    {
        if (!complete(way, pending, size))
            return Verdict::Stop;
        auto const frame = rewrittenGrow(std::span { pending }.first(size), way.channel.kind, rewrite);
        return frame && way.sendTo(way.target(), *frame) ? Verdict::Discard : Verdict::Stop;
    }

    /// Forwards the frame, @p size bytes, @p pending starts with, to Contour with a descriptor of /dev/null
    /// attached; as it is where Contour's end refuses descriptors, and on Windows, where nothing travels
    /// beside a frame.
    /// @return What becomes of the frame.
    [[nodiscard]] Verdict forwardWithDescriptor(Way const& way,
                                                std::vector<uint8_t>& pending,
                                                std::size_t size)
    {
#ifdef _WIN32
        std::ignore = way;
        std::ignore = pending;
        std::ignore = size;
        return Verdict::Forward;
#else
        if (!complete(way, pending, size))
            return Verdict::Stop;
        auto const devNull = transport::OwnedHandle { ::open("/dev/null", O_RDONLY | O_CLOEXEC) };
        if (!devNull)
            return Verdict::Forward;
        auto const lock = std::scoped_lock { way.channel.contourSends };
        auto const sent = way.target().sendWithHandle(std::span { pending }.first(size), devNull.get());
        if (sent)
            return Verdict::Discard;
        // Contour's end refuses descriptors (UpwardDescriptors::Refused): nothing was sent, so the frame
        // goes on as it came, and the host serves on.
        return sent.error() == transport::ChannelError::Refused ? Verdict::Forward : Verdict::Stop;
#endif
    }

    /// @return The MemoryRead whose frame, @p size bytes, @p pending starts with; nothing if it does not
    ///         arrive.
    [[nodiscard]] std::optional<proto::MemoryRead> memoryRead(Way const& way,
                                                              std::vector<uint8_t>& pending,
                                                              std::size_t size)
    {
        if (!complete(way, pending, size))
            return std::nullopt;
        auto const decoded = proto::decodeFrame(pending, way.direction, way.channel.kind, Limits);
        auto const* const read = decoded ? std::get_if<proto::MemoryRead>(&decoded->pdu) : nullptr;
        return read ? std::optional { *read } : std::nullopt;
    }

    /// Does the incident's action on the frame of @p size bytes that @p pending starts with.
    /// @return What becomes of the frame.
    [[nodiscard]] Verdict act(Way const& way, std::vector<uint8_t>& pending, std::size_t size)
    {
        // An answer goes back where the frame came from.
        auto const answer = [&way](std::span<uint8_t const> bytes) {
            return way.sendTo(way.source(), bytes) ? Verdict::Discard : Verdict::Stop;
        };
        auto const objectBytes = _objectBytes.load();
        switch (_incident->action)
        {
            case HostileAction::Abort: crashByAbort();
            case HostileAction::Segfault: crashBySegfault();
            case HostileAction::ForwardThenExit:
                if (complete(way, pending, size))
                    std::ignore = way.sendTo(way.target(), std::span { pending }.first(size));
                std::_Exit(AbruptExitCode);
            case HostileAction::Exit: std::_Exit(AbruptExitCode);
            case HostileAction::ForwardWrongVersion: {
                if (!complete(way, pending, size))
                    return Verdict::Stop;
                auto decoded = proto::decodeFrame(pending, way.direction, way.channel.kind, Limits);
                auto* const hello = decoded ? std::get_if<proto::Hello>(&decoded->pdu) : nullptr;
                if (hello == nullptr)
                    return Verdict::Stop;
                ++hello->version;
                auto const frame = proto::encodeFrame(*hello);
                return frame && way.sendTo(way.target(), *frame) ? Verdict::Discard : Verdict::Stop;
            }
            case HostileAction::StopReadingChannel: return Verdict::Stop;
            case HostileAction::StopReadingEveryChannel:
                _contourReads.store(ContourReads::Stopped);
                return Verdict::Stop;
            case HostileAction::AnswerGarbage: return answer(GarbageAnswer);
            case HostileAction::AnswerOverCapMemory: {
                auto header = proto::Writer {};
                header.varint(1 + Limits.maxMemoryBytes + 17); // The kind, and 17 bytes past the body's cap.
                header.varint(std::to_underlying(proto::PduKind::MemoryData));
                return answer(header.take());
            }
            case HostileAction::AnswerShortMemory: {
                auto const read = memoryRead(way, pending, size);
                if (!read)
                    return Verdict::Stop;
                return answer(memoryDataOf(read->size == 0 ? 1 : read->size - 1));
            }
            case HostileAction::AnswerHalfMemory: {
                auto const read = memoryRead(way, pending, size);
                if (!read)
                    return Verdict::Stop;
                auto const frame = memoryDataOf(read->size);
                auto const header = headerBytesOf(frame);
                return answer(std::span { frame }.first(header + ((frame.size() - header) / 2)));
            }
            case HostileAction::HoldBack: return Verdict::Hold;
            case HostileAction::GrowPastTheObject:
                return forwardGrow(way, pending, size, [objectBytes](proto::Grow& grow) {
                    grow.bytes = objectBytes + proto::WasmPageBytes;
                });
            case HostileAction::GrowToNothing:
                return forwardGrow(way, pending, size, [](proto::Grow& grow) { grow.bytes = 0; });
            case HostileAction::GrowUnaligned:
                return forwardGrow(way, pending, size, [](proto::Grow& grow) { ++grow.bytes; });
            case HostileAction::GrowAnotherInstance:
                return forwardGrow(way, pending, size, [](proto::Grow& grow) { ++grow.instanceId; });
            case HostileAction::ForwardWithDescriptor: return forwardWithDescriptor(way, pending, size);
            case HostileAction::ExitAsMemoryUnusable:
                std::_Exit(vtextension::wasm::host::exitCodeOf(
                    vtextension::wasm::host::HostProcessExit::MemoryUnusable));
        }
        return Verdict::Forward;
    }

    /// @return Whether @p way is one this process no longer reads.
    [[nodiscard]] bool isStopped(Way const& way) const noexcept
    {
        return way.direction == proto::Direction::ToHelper && _contourReads.load() == ContourReads::Stopped;
    }

    /// Reads @p way and passes nothing on, until its source ends; then ends its target's stream too.
    static void hold(Way const& way)
    {
        auto chunk = std::array<uint8_t, 4096> {};
        while (way.source().receiveSome(chunk))
        {
        }
        way.target().shutdown();
    }

    /// Relays @p channel the way @p direction says, frame by frame, until either end's stream ends -- then
    /// the other's ends too -- or the incident stops it.
    void pump(RelayedChannel& channel, proto::Direction direction)
    {
        auto const way = Way { .channel = channel, .direction = direction };
        auto pending = std::vector<uint8_t> {}; // Received, not passed on yet.
        auto frameLeft = std::size_t { 0 };     // Bytes of the current frame not passed on yet.
        auto verdict = Verdict::Forward;        // What becomes of them.
        auto chunk = std::array<uint8_t, 4096> {};
        while (true)
        {
            if (frameLeft == 0 && !pending.empty())
            {
                auto const size = proto::frameSizeOf(pending, direction, channel.kind, Limits);
                if (size)
                {
                    auto const kind = kindAtFront(pending);
                    if (isIncident(way, kind))
                        verdict = act(way, pending, *size);
                    else if (isGranting(way) && kind == proto::PduKind::Instantiate)
                        verdict = relayInstantiate(way, pending, *size);
                    else
                        verdict = Verdict::Forward;
                    _passed.fetch_or(bitOf(kind));
                    if (verdict == Verdict::Stop)
                        return;
                    if (verdict == Verdict::Hold)
                    {
                        hold(way);
                        return;
                    }
                    frameLeft = *size;
                }
                else if (size.error() != proto::DecodeError::NeedMoreData)
                {
                    verdict = Verdict::Forward; // Passed on as it is: the receiver refuses it.
                    frameLeft = pending.size();
                }
            }
            if (frameLeft > 0 && !pending.empty())
            {
                auto const count = std::min(frameLeft, pending.size());
                if (verdict == Verdict::Forward
                    && !way.sendTo(way.target(), std::span { pending }.first(count)))
                    return;
                pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(count));
                frameLeft -= count;
                continue;
            }
            if (isStopped(way))
                return;
            auto const received = receiveOn(way, chunk);
            if (!received)
            {
                way.target().shutdown();
                return;
            }
            // What arrived once Contour's channels are no longer read is passed on to nobody.
            if (isStopped(way))
                return;
            pending.insert(
                pending.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(*received));
        }
    }

    std::optional<HostileIncident> _incident;
    GrantedMemory _granted;
    std::vector<std::unique_ptr<RelayedChannel>> _channels;
    std::atomic<uint64_t> _objectBytes { 0 }; ///< The object size of the latest Instantiate's grant.
    /// Descriptors Contour granted, not forwarded yet: read and taken by the control channel's pump alone.
    std::deque<transport::OwnedHandle> _grantedHandles;
    /// Scribbles on the latest grant where the row says so; stopped and joined after the pumps.
    std::optional<Scribbler> _scribbler;
    std::atomic<Progress> _progress { Progress::Pending };
    std::atomic<ContourReads> _contourReads { ContourReads::Open };
    std::atomic<uint32_t> _passed { 0 }; ///< One bit per kind of frame passed so far, either way.
    std::vector<std::jthread> _pumps;    // Last, so that they are joined before the channels go.
};

/// Puts a relay between Contour's end of a channel and the real host.
/// @param kind The channel's kind.
/// @param handle Contour's end, as this process inherited it; replaced by the end the real host is handed.
/// @return Contour's end, and this process's end of a fresh channel the real host serves in its place;
///         nothing if either cannot be had.
[[nodiscard]] std::unique_ptr<RelayedChannel> relayed(proto::ChannelKind kind,
                                                      transport::NativeHandle& handle)
{
    auto contour = transport::Channel::adopt(handle);
    if (!contour)
        return nullptr;
    auto inner = transport::makeChannelPair();
    if (!inner)
        return nullptr;
    auto const copy = transport::testing::duplicate(inner->helper.nativeHandle());
    if (!copy)
        return nullptr;
    handle = *copy;
    {
        // Only the host's copy stays open: its end of stream is the relay's.
        auto const original = std::move(inner->helper);
    }
    // Built in place: its mutex moves nowhere.
    return std::make_unique<RelayedChannel>(kind, *std::move(contour), std::move(inner->parent));
}

/// Runs the real host as @p row says, on @p arguments: with the wake signal it names, behind a relay if it
/// names an incident, and ending as it says once Contour's channels ended.
[[nodiscard]] int misbehave(HostileModeRow const& row, std::vector<std::string_view> const& arguments)
{
    auto parsed = transport::parseHelperArguments(arguments);
    if (!parsed)
        return serve({ arguments.begin(), arguments.end() });

    // Made whether used or not, so that it lives as long as the host it is handed to.
    auto const silent = transport::makeWakePair();
    if (row.wake == WakeSource::Silent)
    {
        if (!silent)
            return refuse("no wake signal of its own can be made");
        auto const copy = transport::testing::duplicate(silent->helper.nativeHandle());
        if (!copy)
            return refuse("no wake signal of its own can be handed over");
        parsed->wake = *copy;
    }

    auto relay = std::optional<Relay> {};
    if (row.incident || row.granted == GrantedMemory::Scribbled)
    {
        auto channels = std::vector<std::unique_ptr<RelayedChannel>> {};
        channels.push_back(relayed(proto::ChannelKind::Control, parsed->control));
        for (auto& lane: parsed->lanes)
            channels.push_back(relayed(proto::ChannelKind::Lane, lane));
        if (std::ranges::any_of(channels, [](auto const& channel) { return channel == nullptr; }))
            return refuse("a channel cannot be relayed");
        relay.emplace(row.incident, row.granted, std::move(channels));
    }

    auto const served = serve(transport::formatHelperArguments(*parsed), row.memory);
    if (served != 0 || row.afterEnd == AfterContourEnds::Exit)
        return served;
#ifndef _WIN32
    std::signal(SIGTERM, SIG_IGN);
#endif
    while (true)
        std::this_thread::sleep_for(std::chrono::hours { 1 });
}

} // namespace

int main(int argc, char const* argv[])
{
    auto arguments = std::vector<std::string_view> {};
    auto const* row = static_cast<HostileModeRow const*>(nullptr);
    for (auto const argument: std::vector<std::string_view>(argv + 1, argv + argc))
    {
        if (!argument.starts_with(HostileOption))
        {
            arguments.push_back(argument);
            continue;
        }
        row = hostileModeNamed(argument.substr(HostileOption.size()));
        if (row == nullptr)
            return refuse("no such hostile mode");
    }
    if (row == nullptr)
        return serve({ arguments.begin(), arguments.end() });
    return misbehave(*row, arguments);
}
