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
#include <expected>
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
#include <vtextension/wasm/remote/proto/Codec.hpp>
#include <vtextension/wasm/remote/proto/Pdu.hpp>
#include <vtextension/wasm/remote/proto/Wire.hpp>
#include <vtextension/wasm/remote/transport/Channel.hpp>
#include <vtextension/wasm/remote/transport/HelperArguments.hpp>
#include <vtextension/wasm/remote/transport/WakeSignal.hpp>
#include <vtextension/wasm/remote/transport/testing/HandleCopies.hpp>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <Windows.h>
#endif

namespace proto = vtextension::wasm::remote::proto;
namespace transport = vtextension::wasm::remote::transport;
using contour::extension_host::testing::AfterContourEnds;
using contour::extension_host::testing::GarbageAnswer;
using contour::extension_host::testing::HostileAction;
using contour::extension_host::testing::HostileIncident;
using contour::extension_host::testing::hostileModeNamed;
using contour::extension_host::testing::HostileModeRow;
using contour::extension_host::testing::HostileOption;
using contour::extension_host::testing::WakeSource;

namespace
{

/// The exit code of a hostile host whose own command line is wrong: none of the real host's codes.
constexpr auto HostileSetupFailed = 76;

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

/// Runs the real host on @p arguments.
[[nodiscard]] int serve(std::vector<std::string> const& arguments)
{
    auto views = std::vector<std::string_view> {};
    for (auto const& argument: arguments)
        views.emplace_back(argument);
    return vtextension::wasm::host::runExtensionHost(views, &vtextension::wasm::host::makeHelperEngine);
}

/// Crashes as std::abort() does, with no dialog on Windows.
[[noreturn]] void crashByAbort()
{
#ifdef _WIN32
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    std::abort();
}

/// Dies as a segmentation fault does, past any handler a sanitizer runtime installed.
[[noreturn]] void crashBySegfault()
{
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

/// Relays every channel between Contour's end and the real host's, both ways, frame by frame -- until the
/// incident's trigger, once, where its action happens.
class Relay
{
  public:
    /// @param incident What happens, and where.
    /// @param channels Every channel relayed; the first is the control channel.
    Relay(HostileIncident incident, std::vector<std::unique_ptr<RelayedChannel>> channels):
        _incident { incident }, _channels { std::move(channels) }
    {
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
        auto const& trigger = _incident.trigger;
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

    /// Receives on @p way until @p pending holds @p size bytes.
    /// @return Whether they arrived.
    [[nodiscard]] static bool complete(Way const& way, std::vector<uint8_t>& pending, std::size_t size)
    {
        auto chunk = std::array<uint8_t, 4096> {};
        while (pending.size() < size)
        {
            auto const received = way.source().receiveSome(chunk);
            if (!received)
                return false;
            pending.insert(
                pending.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(*received));
        }
        return true;
    }

    /// @return The MemoryRead whose frame, @p size bytes, @p pending starts with; nothing if it does not
    ///         arrive.
    [[nodiscard]] static std::optional<proto::MemoryRead> memoryRead(Way const& way,
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
        switch (_incident.action)
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
                    verdict = isIncident(way, kind) ? act(way, pending, *size) : Verdict::Forward;
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
            auto const received = way.source().receiveSome(chunk);
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

    HostileIncident _incident;
    std::vector<std::unique_ptr<RelayedChannel>> _channels;
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
    if (row.incident)
    {
        auto channels = std::vector<std::unique_ptr<RelayedChannel>> {};
        channels.push_back(relayed(proto::ChannelKind::Control, parsed->control));
        for (auto& lane: parsed->lanes)
            channels.push_back(relayed(proto::ChannelKind::Lane, lane));
        if (std::ranges::any_of(channels, [](auto const& channel) { return channel == nullptr; }))
            return refuse("a channel cannot be relayed");
        relay.emplace(*row.incident, std::move(channels));
    }

    auto const served = serve(transport::formatHelperArguments(*parsed));
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
