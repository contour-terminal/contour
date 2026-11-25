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
#include <expected>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
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

namespace proto = vtextension::wasm::remote::proto;
namespace transport = vtextension::wasm::remote::transport;
using contour::extension_host::testing::HostileMode;
using contour::extension_host::testing::hostileModeNamed;
using contour::extension_host::testing::HostileOption;

namespace
{

/// The exit code of a hostile host whose own command line is wrong: none of the real host's codes.
constexpr auto HostileSetupFailed = 76;

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

/// Runs the real host with a wake signal of its own that nobody signals, in place of the one Contour hands
/// it: the host's listener never wakes, so the page's flags are never read. Contour's end stays open and
/// unread, as a stuck host's would.
[[nodiscard]] int ignoreInterrupts(std::vector<std::string_view> const& arguments)
{
    auto parsed = transport::parseHelperArguments(arguments);
    if (!parsed)
        return serve({ arguments.begin(), arguments.end() });
    auto silent = transport::makeWakePair();
    if (!silent)
        return refuse("no wake signal of its own can be made");
    auto const copy = transport::testing::duplicate(silent->helper.nativeHandle());
    if (!copy)
        return refuse("no wake signal of its own can be handed over");
    parsed->wake = *copy;
    return serve(transport::formatHelperArguments(*parsed));
}

/// Relays the control channel between Contour's end and the real host's, frame by frame: Contour's frames
/// on to the host until, once the host answered an instantiation, Contour sends a Load, at whose header the
/// relay stops reading for good; and the host's frames back as they are.
class StoppingRelay
{
  public:
    /// @param contour Contour's control channel, as this process inherited it.
    /// @param host This process's end of the channel the real host serves.
    StoppingRelay(transport::Channel contour, transport::Channel host):
        _contour { std::move(contour) },
        _host { std::move(host) },
        _toHost { [this] { relayToHost(); } },
        _toContour { [this] { relayToContour(); } }
    {
    }
    StoppingRelay(StoppingRelay const&) = delete;
    StoppingRelay(StoppingRelay&&) = delete;
    StoppingRelay& operator=(StoppingRelay const&) = delete;
    StoppingRelay& operator=(StoppingRelay&&) = delete;
    ~StoppingRelay() = default;

  private:
    /// Whether the host answered an instantiation yet.
    enum class Progress : uint8_t
    {
        NotInstantiated,
        Instantiated,
    };

    /// The limits both ends apply.
    static constexpr auto Limits = proto::PduLimits {};

    /// @return The kind of the frame whose header @p buffer starts with; frameSizeOf() accepted it.
    [[nodiscard]] static std::optional<proto::PduKind> kindAtFront(std::span<uint8_t const> buffer)
    {
        auto header = proto::Reader { buffer };
        if (!header.varint64())
            return std::nullopt;
        auto const kind = header.u8();
        return kind ? std::optional { static_cast<proto::PduKind>(*kind) } : std::nullopt;
    }

    void relayToHost()
    {
        auto pending = std::vector<uint8_t> {}; // received, not forwarded yet
        auto frameLeft = std::size_t { 0 };     // bytes of the current frame not forwarded yet
        auto chunk = std::array<uint8_t, 4096> {};
        while (true)
        {
            if (frameLeft == 0 && !pending.empty())
            {
                auto const size = proto::frameSizeOf(
                    pending, proto::Direction::ToHelper, proto::ChannelKind::Control, Limits);
                if (size)
                {
                    if (_progress.load() == Progress::Instantiated
                        && kindAtFront(pending) == proto::PduKind::Load)
                        return; // reads nothing more: Contour's send blocks
                    frameLeft = *size;
                }
                else if (size.error() != proto::DecodeError::NeedMoreData)
                    frameLeft = pending.size(); // passed on as it is: the host refuses it
            }
            if (frameLeft > 0 && !pending.empty())
            {
                auto const count = std::min(frameLeft, pending.size());
                if (!_host.send(std::span { pending }.first(count)))
                    return;
                pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(count));
                frameLeft -= count;
                continue;
            }
            auto const received = _contour.receiveSome(chunk);
            if (!received)
            {
                _host.shutdown();
                return;
            }
            pending.insert(
                pending.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(*received));
        }
    }

    void relayToContour()
    {
        auto pending = std::vector<uint8_t> {};
        auto chunk = std::array<uint8_t, 4096> {};
        while (true)
        {
            auto decoded =
                proto::decodeFrame(pending, proto::Direction::ToParent, proto::ChannelKind::Control, Limits);
            if (decoded)
            {
                auto const frame = std::span { pending }.first(decoded->consumed);
                if (!_contour.send(frame))
                    return;
                if (std::holds_alternative<proto::Instantiated>(decoded->pdu))
                    _progress.store(Progress::Instantiated);
                pending.erase(pending.begin(),
                              pending.begin() + static_cast<std::ptrdiff_t>(decoded->consumed));
                continue;
            }
            auto const received = decoded.error() == proto::DecodeError::NeedMoreData
                                      ? _host.receiveSome(chunk)
                                      : std::expected<std::size_t, transport::ChannelError> { std::unexpected(
                                            transport::ChannelError::Io) };
            if (!received)
            {
                _contour.shutdown();
                return;
            }
            pending.insert(
                pending.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(*received));
        }
    }

    transport::Channel _contour;
    transport::Channel _host;
    std::atomic<Progress> _progress { Progress::NotInstantiated };
    std::jthread _toHost; // Last, so that they are joined before the channels go.
    std::jthread _toContour;
};

/// Runs the real host behind a StoppingRelay on its control channel; its lanes, page and wake signal are
/// Contour's own.
[[nodiscard]] int stopReadingAfterInstantiate(std::vector<std::string_view> const& arguments)
{
    auto parsed = transport::parseHelperArguments(arguments);
    if (!parsed)
        return serve({ arguments.begin(), arguments.end() });
    auto contour = transport::Channel::adopt(parsed->control);
    if (!contour)
        return refuse("Contour's control channel cannot be adopted");
    auto inner = transport::makeChannelPair();
    if (!inner)
        return refuse("no channel to relay to can be made");
    auto const copy = transport::testing::duplicate(inner->helper.nativeHandle());
    if (!copy)
        return refuse("no channel to relay to can be handed over");
    parsed->control = *copy;
    {
        // Only the host's copy stays open: its end of stream is the relay's.
        auto const original = std::move(inner->helper);
    }
    auto const relay = StoppingRelay { *std::move(contour), std::move(inner->parent) };
    return serve(transport::formatHelperArguments(*parsed));
}

/// Runs the real host, and once it ended because Contour's channels did, sleeps until killed, SIGTERM
/// ignored. A host that ended for another reason ends as the real one does.
[[nodiscard]] int ignoreEof(std::vector<std::string_view> const& arguments)
{
    if (auto const served = serve({ arguments.begin(), arguments.end() }); served != 0)
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
    auto mode = std::optional<HostileMode> {};
    for (auto const argument: std::vector<std::string_view>(argv + 1, argv + argc))
    {
        if (!argument.starts_with(HostileOption))
        {
            arguments.push_back(argument);
            continue;
        }
        mode = hostileModeNamed(argument.substr(HostileOption.size()));
        if (!mode)
            return refuse("no such hostile mode");
    }
    if (!mode)
        return refuse("no hostile mode given");
    switch (*mode)
    {
        case HostileMode::IgnoreInterrupt: return ignoreInterrupts(arguments);
        case HostileMode::StopReadingAfterInstantiate: return stopReadingAfterInstantiate(arguments);
        case HostileMode::IgnoreEof: return ignoreEof(arguments);
    }
    return refuse("no such hostile mode");
}
