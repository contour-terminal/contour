// SPDX-License-Identifier: Apache-2.0
//
// The backend that runs extensions in a separate host: the contract every backend meets, run through hosts
// serving in this process; and what only a remote backend does, against real hosts and against a test that
// plays the host itself.
#include <core/Clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <functional>
#include <future>
#include <latch>
#include <memory>
#include <mutex>
#include <optional>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <vtextension/ExtensionError.hpp>
#include <vtextension/testing/TestModules.hpp>
#include <vtextension/wasm/BackendConfig.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>
#include <vtextension/wasm/host/HostServer.hpp>
#include <vtextension/wasm/host/InProcessHost.hpp>
#include <vtextension/wasm/remote/HelperIncarnation.hpp>
#include <vtextension/wasm/remote/HostConnector.hpp>
#include <vtextension/wasm/remote/HostRefusals.hpp>
#include <vtextension/wasm/remote/RemoteBackend.hpp>
#include <vtextension/wasm/remote/proto/Codec.hpp>
#include <vtextension/wasm/remote/proto/Pdu.hpp>
#include <vtextension/wasm/remote/transport/Channel.hpp>
#include <vtextension/wasm/remote/transport/ControlPage.hpp>
#include <vtextension/wasm/remote/transport/PduStream.hpp>
#include <vtextension/wasm/testing/BackendContract.hpp>
#include <vtextension/wasm/testing/HostedEngine.hpp>
#include <vtextension/wasm/testing/RenderMeasurement.hpp>

using vtextension::ExtensionError;
using vtextension::testing::toModule;
using vtextension::wasm::BackendConfig;
using vtextension::wasm::CallFailure;
using vtextension::wasm::ExportDescription;
using vtextension::wasm::ExternKind;
using vtextension::wasm::FunctionSignature;
using vtextension::wasm::IHostContext;
using vtextension::wasm::ImportDescription;
using vtextension::wasm::IWasmBackend;
using vtextension::wasm::IWasmInstance;
using vtextension::wasm::IWasmModule;
using vtextension::wasm::IWasmRuntime;
using vtextension::wasm::Linkage;
using vtextension::wasm::LockdownRequirement;
using vtextension::wasm::MemoryFailure;
using vtextension::wasm::ModuleIdentity;
using vtextension::wasm::ValueType;
using vtextension::wasm::host::HostServerConfig;
using vtextension::wasm::host::InProcessHostConnector;
using vtextension::wasm::remote::HelperIncarnation;
using vtextension::wasm::remote::HelperPolicy;
using vtextension::wasm::remote::HostAttachment;
using vtextension::wasm::remote::HostConnection;
using vtextension::wasm::remote::HostRefusal;
using vtextension::wasm::remote::HostRefusalLog;
using vtextension::wasm::remote::hostRefusals;
using vtextension::wasm::remote::HostStep;
using vtextension::wasm::remote::IHostConnector;
using vtextension::wasm::remote::IHostHandle;
using vtextension::wasm::remote::LaneArming;
using vtextension::wasm::remote::LaneWatch;
using vtextension::wasm::remote::makeRemoteBackend;
using vtextension::wasm::remote::makeRemoteKindState;
using vtextension::wasm::remote::MaxLaneChannels;
using vtextension::wasm::remote::RemoteKindState;
using vtextension::wasm::remote::StepWatch;
using vtextension::wasm::remote::transport::Channel;
using vtextension::wasm::remote::transport::makeChannelPair;
using vtextension::wasm::remote::transport::PduStream;
using vtextension::wasm::testing::makeHostedEngine;

namespace proto = vtextension::wasm::remote::proto;
namespace contract = vtextension::wasm::testing::contract;

namespace
{

/// The kind every remote backend of this binary shares, as a process's do.
[[nodiscard]] std::shared_ptr<RemoteKindState> const& sharedKind()
{
    static auto const kind = makeRemoteKindState();
    return kind;
}

/// @return A connector starting each host in this process, on an engine of its own.
[[nodiscard]] std::shared_ptr<InProcessHostConnector> makeConnector()
{
    return std::make_shared<InProcessHostConnector>([] { return makeHostedEngine(); }, HostServerConfig {});
}

/// @return A remote backend of the shared kind, starting its hosts through @p connector and driving those
///         confined as @p lockdown asks: by default also those without a sandbox, which every host here is.
[[nodiscard]] std::unique_ptr<IWasmBackend> makeBackend(
    std::shared_ptr<IHostConnector> connector,
    LockdownRequirement lockdown = LockdownRequirement::AcceptUnsandboxed)
{
    return makeRemoteBackend(
        sharedKind(),
        BackendConfig { .timeouts = {}, .connector = std::move(connector), .lockdown = lockdown });
}

struct RemoteTraits
{
    [[nodiscard]] static std::unique_ptr<IWasmBackend> make() { return makeBackend(makeConnector()); }
};

/// Reads and writes its memory around a host call: `load(a)` returns the word at `a`; `ping-then-load(a)`
/// calls `ping(7)`, then returns the word at `a`.
///
///     (module
///       (import "contract:test/host@0" "ping" (func $ping (param i32)))
///       (memory (export "memory") 1)
///       (func (export "load") (param i32) (result i32) (i32.load (local.get 0)))
///       (func (export "ping-then-load") (param i32) (result i32)
///         (call $ping (i32.const 7)) (i32.load (local.get 0))))
constexpr auto PeekModule = std::to_array<uint8_t>({
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x0a, 0x02, 0x60, 0x01, 0x7f, 0x00,
    0x60, 0x01, 0x7f, 0x01, 0x7f, 0x02, 0x1d, 0x01, 0x14, 0x63, 0x6f, 0x6e, 0x74, 0x72, 0x61,
    0x63, 0x74, 0x3a, 0x74, 0x65, 0x73, 0x74, 0x2f, 0x68, 0x6f, 0x73, 0x74, 0x40, 0x30, 0x04,
    0x70, 0x69, 0x6e, 0x67, 0x00, 0x00, 0x03, 0x03, 0x02, 0x01, 0x01, 0x05, 0x03, 0x01, 0x00,
    0x01, 0x07, 0x22, 0x03, 0x06, 0x6d, 0x65, 0x6d, 0x6f, 0x72, 0x79, 0x02, 0x00, 0x04, 0x6c,
    0x6f, 0x61, 0x64, 0x00, 0x01, 0x0e, 0x70, 0x69, 0x6e, 0x67, 0x2d, 0x74, 0x68, 0x65, 0x6e,
    0x2d, 0x6c, 0x6f, 0x61, 0x64, 0x00, 0x02, 0x0a, 0x15, 0x02, 0x07, 0x00, 0x20, 0x00, 0x28,
    0x02, 0x00, 0x0b, 0x0b, 0x00, 0x41, 0x07, 0x10, 0x00, 0x20, 0x00, 0x28, 0x02, 0x00, 0x0b,
});

/// @return PeekModule's description, as a host describes it.
[[nodiscard]] proto::Loaded peekDescription(uint32_t moduleId)
{
    auto const wordToWord =
        FunctionSignature { .parameters = { ValueType::I32 }, .results = { ValueType::I32 } };
    return proto::Loaded {
        .moduleId = moduleId,
        .error = std::nullopt,
        .imports = { ImportDescription { .module = std::string { contract::HostModule },
                                         .name = "ping",
                                         .kind = ExternKind::Function,
                                         .signature = { .parameters = { ValueType::I32 }, .results = {} },
                                         .linkage = Linkage::Satisfied } },
        .exports = { ExportDescription {
                         .name = "memory", .kind = ExternKind::Memory, .signature = {}, .initialPages = 1 },
                     ExportDescription { .name = "load",
                                         .kind = ExternKind::Function,
                                         .signature = wordToWord,
                                         .initialPages = 0 },
                     ExportDescription { .name = "ping-then-load",
                                         .kind = ExternKind::Function,
                                         .signature = wordToWord,
                                         .initialPages = 0 } },
    };
}

/// @return @p bytes loaded on @p runtime under @p key, which must succeed.
template <std::size_t N>
[[nodiscard]] std::shared_ptr<IWasmModule> loadAs(IWasmRuntime& runtime,
                                                  std::string key,
                                                  std::array<uint8_t, N> const& bytes)
{
    auto module = runtime.load(ModuleIdentity { std::move(key) }, toModule(bytes));
    REQUIRE(module.has_value());
    return std::move(*module);
}

/// @return What calling @p instance's @p name with @p argument returns, which must succeed.
[[nodiscard]] uint32_t callWord(IWasmInstance& instance, std::string_view name, uint32_t argument)
{
    auto argv = std::array { argument };
    auto const called = instance.call(contract::functionOf(instance, name), argv);
    REQUIRE(called.has_value());
    return argv[0];
}

/// @return What calling @p instance's `add(2, 3)` fails with; nothing if it returned 5.
[[nodiscard]] std::optional<CallFailure> addFailure(IWasmInstance& instance)
{
    auto argv = std::array<uint32_t, 2> { 2, 3 };
    auto const called = instance.call(contract::functionOf(instance, "add"), argv);
    if (!called)
        return called.error();
    CHECK(argv[0] == 5);
    return std::nullopt;
}

/// Writes @p bytes at @p address of @p instance's memory, through a view.
void writeAt(IWasmInstance& instance, uint32_t address, std::span<uint8_t const> bytes)
{
    auto const view = instance.memory(address, static_cast<uint32_t>(bytes.size()));
    REQUIRE(view.has_value());
    std::ranges::transform(bytes, view->begin(), [](uint8_t byte) { return std::byte { byte }; });
}

/// A host context whose host calls wait until the test releases them, each counting down a latch first.
class ParkingHostContext final: public IHostContext
{
  public:
    ParkingHostContext(std::latch& parked, std::shared_future<void> release) noexcept:
        _parked { parked }, _release { std::move(release) }
    {
    }

    [[nodiscard]] std::expected<void, ExtensionError> callHost(
        std::size_t /*functionIndex*/, std::span<uint32_t const> /*arguments*/) noexcept override
    {
        _parked.count_down();
        _release.wait();
        return {};
    }

  private:
    std::latch& _parked;
    std::shared_future<void> _release;
};

/// Calls `ping-then-store` of each of @p instances on a prepared thread of its own, each parking in its host
/// call until @p release is set; destroyed, it joins them.
class ParkedCalls
{
  public:
    ParkedCalls(IWasmBackend& backend, std::span<std::unique_ptr<IWasmInstance> const> instances)
    {
        for (auto const& instance: instances)
            _threads.emplace_back([&backend, &instance = *instance, this] {
                auto const guard = backend.enterThread();
                if (guard)
                    std::ignore = instance.call(*instance.function("ping-then-store"), {});
                else
                    _failedGuards.fetch_add(1);
            });
    }

    ParkedCalls(ParkedCalls const&) = delete;
    ParkedCalls(ParkedCalls&&) = delete;
    ParkedCalls& operator=(ParkedCalls const&) = delete;
    ParkedCalls& operator=(ParkedCalls&&) = delete;
    ~ParkedCalls() = default;

    /// @return How many threads could not be prepared.
    [[nodiscard]] int failedGuards() const noexcept { return _failedGuards.load(); }

  private:
    std::atomic<int> _failedGuards { 0 };
    std::vector<std::jthread> _threads;
};

/// A host context whose host calls write 9, 8, 7, 6 at Address of the instance attached to it.
class WritingHostContext final: public IHostContext
{
  public:
    void attach(IWasmInstance& instance) noexcept { _instance = &instance; }

    [[nodiscard]] std::expected<void, ExtensionError> callHost(
        std::size_t /*functionIndex*/, std::span<uint32_t const> /*arguments*/) noexcept override
    {
        auto const view = _instance->memory(Address, 4);
        if (!view)
            return std::unexpected(ExtensionError::Trapped);
        std::ranges::copy(
            std::to_array({ std::byte { 9 }, std::byte { 8 }, std::byte { 7 }, std::byte { 6 } }),
            view->begin());
        return {};
    }

    static constexpr auto Address = uint32_t { 64 };

  private:
    IWasmInstance* _instance = nullptr;
};

/// @return @p pdu as one frame; empty if it cannot be encoded.
[[nodiscard]] std::vector<uint8_t> frameOf(proto::Pdu const& pdu)
{
    return proto::encodeFrame(pdu).value_or(std::vector<uint8_t> {});
}

/// @return The header of a frame of @p kind whose body is @p bodyBytes long, without the body.
[[nodiscard]] std::vector<uint8_t> headerOf(proto::PduKind kind, uint64_t bodyBytes)
{
    auto header = std::vector<uint8_t> {};
    auto length = bodyBytes + 1; // the body, and the kind's byte
    do
    {
        auto const low = static_cast<uint8_t>(length & 0x7f);
        length >>= 7;
        header.push_back(static_cast<uint8_t>(low | (length != 0 ? 0x80 : 0x00)));
    } while (length != 0);
    header.push_back(static_cast<uint8_t>(kind));
    return header;
}

/// The helper's ends of a scripted connection, all raw, to write what no PduStream would.
struct HelperEnds
{
    Channel control;               ///< The control channel.
    std::vector<Channel> lanes;    ///< The lanes.
    std::vector<uint8_t> received; ///< Control bytes received and not decoded yet.

    /// Sends @p frame on the control channel.
    /// @return Whether it was sent.
    [[nodiscard]] bool sendFrame(std::span<uint8_t const> frame) { return control.send(frame).has_value(); }

    /// Sends @p pdu on the control channel.
    /// @return Whether it was sent.
    [[nodiscard]] bool send(proto::Pdu const& pdu) { return sendFrame(frameOf(pdu)); }

    /// @return The next control PDU; nothing once the channel closed or carried what the codec refuses.
    [[nodiscard]] std::optional<proto::Pdu> receivePdu()
    {
        while (true)
        {
            auto decoded = proto::decodeFrame(
                received, proto::Direction::ToHelper, proto::ChannelKind::Control, proto::PduLimits {});
            if (decoded)
            {
                received.erase(received.begin(),
                               received.begin() + static_cast<std::ptrdiff_t>(decoded->consumed));
                return std::move(decoded->pdu);
            }
            if (decoded.error() != proto::DecodeError::NeedMoreData)
                return std::nullopt;
            auto chunk = std::array<uint8_t, 4096> {};
            auto const count = control.receiveSome(chunk);
            if (!count)
                return std::nullopt;
            received.insert(
                received.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(*count));
        }
    }

    /// @return The next control PDU, if it is a @p T; nothing otherwise.
    template <typename T>
    [[nodiscard]] std::optional<T> receive()
    {
        auto pdu = receivePdu();
        if (!pdu || !std::holds_alternative<T>(*pdu))
            return std::nullopt;
        return std::get<T>(std::move(*pdu));
    }

    /// Writes @p bytes on every lane.
    void writeOnEveryLane(std::span<uint8_t const> bytes)
    {
        for (auto& lane: lanes)
            std::ignore = lane.send(bytes);
    }

    /// Waits until the parent closes its end of the control channel.
    void drain()
    {
        while (receivePdu())
        {
        }
    }
};

/// One step's watch a host handle was asked for.
struct WatchRecord
{
    HostStep step;
    std::chrono::milliseconds budget;

    bool operator==(WatchRecord const&) const = default;
};

/// What the handles of one connector record: terminations, the watches armed and disarmed, and the
/// attachment of the latest connection.
struct HandleRecords
{
    std::atomic<int> terminations = 0;
    std::atomic<int> lettings = 0; ///< How often a handle was let go.
    std::atomic<std::size_t> connections = 0;
    std::mutex mutex;                 ///< Guards what follows.
    std::vector<WatchRecord> watches; ///< Every watch armed, in order.
    std::size_t disarmed = 0;         ///< How many watches were disarmed.
    HostAttachment attachment;        ///< The latest connection's.
    /// Run once, by the next watch disarmed, outside the mutex: where a request still holds the control
    /// channel after its last send.
    std::function<void()> onNextDisarm;
};

/// A host handle that records what it is told.
class CountingHandle final: public IHostHandle
{
  public:
    explicit CountingHandle(std::shared_ptr<HandleRecords> records) noexcept: _records { std::move(records) }
    {
    }

    void interrupt(uint32_t /*interruptSlot*/) noexcept override {}
    void clearInterrupt(uint32_t /*interruptSlot*/) noexcept override {}
    void terminate() noexcept override { _records->terminations.fetch_add(1); }
    void letGo() noexcept override { _records->lettings.fetch_add(1); }

    [[nodiscard]] StepWatch watch(HostStep step, std::chrono::milliseconds budget) override
    {
        auto const lock = std::scoped_lock { _records->mutex };
        _records->watches.push_back(WatchRecord { .step = step, .budget = budget });
        return StepWatch { [records = _records] {
            auto hook = std::function<void()> {};
            {
                auto const disarmLock = std::scoped_lock { records->mutex };
                ++records->disarmed;
                hook = std::exchange(records->onNextDisarm, nullptr);
            }
            if (hook)
                hook();
        } };
    }

    void attach(HostAttachment attachment) override
    {
        auto const lock = std::scoped_lock { _records->mutex };
        _records->attachment = std::move(attachment);
    }

  private:
    std::shared_ptr<HandleRecords> _records;
};

/// A connector whose host is the test: each connection runs the script on the helper's ends, on a thread of
/// its own, which is joined when the connector goes. A script returns once the parent closes its ends.
class ScriptedConnector final: public IHostConnector
{
  public:
    /// What a connection's host does, given the helper's ends and how many connections came before it.
    using Script = std::function<void(HelperEnds&, std::size_t)>;

    explicit ScriptedConnector(Script script): _script { std::move(script) } {}
    ScriptedConnector(ScriptedConnector const&) = delete;
    ScriptedConnector(ScriptedConnector&&) = delete;
    ScriptedConnector& operator=(ScriptedConnector const&) = delete;
    ScriptedConnector& operator=(ScriptedConnector&&) = delete;
    ~ScriptedConnector() override = default;

    [[nodiscard]] std::expected<HostConnection, ExtensionError> connect() override
    {
        auto control = makeChannelPair();
        if (!control)
            return std::unexpected(ExtensionError::HostUnavailable);
        auto connection = HostConnection { .control = std::move(control->parent),
                                           .lanes = {},
                                           .handle = std::make_unique<CountingHandle>(_records) };
        auto ends = HelperEnds { .control = std::move(control->helper), .lanes = {}, .received = {} };
        for ([[maybe_unused]] auto const index: std::views::iota(std::size_t { 0 }, MaxLaneChannels))
        {
            auto lane = makeChannelPair();
            if (!lane)
                return std::unexpected(ExtensionError::HostUnavailable);
            connection.lanes.push_back(std::move(lane->parent));
            ends.lanes.push_back(std::move(lane->helper));
        }
        _records->connections.fetch_add(1);
        _threads.emplace_back([script = _script,
                               ends = std::move(ends),
                               connection = _threads.size()]() mutable { script(ends, connection); });
        return connection;
    }

    /// @return How often a host of this connector was told to terminate.
    [[nodiscard]] int terminations() const noexcept { return _records->terminations.load(); }

    /// @return How often a host of this connector was let go before its handle went.
    [[nodiscard]] int lettings() const noexcept { return _records->lettings.load(); }

    /// @return How many hosts it connected.
    [[nodiscard]] std::size_t connections() const noexcept { return _records->connections.load(); }

    /// @return Every watch its hosts' handles armed, in order.
    [[nodiscard]] std::vector<WatchRecord> watches() const
    {
        auto const lock = std::scoped_lock { _records->mutex };
        return _records->watches;
    }

    /// @return How many of them were disarmed.
    [[nodiscard]] std::size_t disarmed() const
    {
        auto const lock = std::scoped_lock { _records->mutex };
        return _records->disarmed;
    }

    /// Has the next watch disarmed run @p hook, once.
    void onNextDisarm(std::function<void()> hook)
    {
        auto const lock = std::scoped_lock { _records->mutex };
        _records->onNextDisarm = std::move(hook);
    }

    /// @return The latest connection's attachment.
    [[nodiscard]] HostAttachment attachment() const
    {
        auto const lock = std::scoped_lock { _records->mutex };
        return _records->attachment;
    }

  private:
    Script _script;
    std::shared_ptr<HandleRecords> _records = std::make_shared<HandleRecords>();
    std::vector<std::jthread> _threads;
};

/// The Hello an honest helper opens with.
constexpr auto HonestHello = proto::Hello { .version = proto::ProtocolVersion,
                                            .memoryMode = proto::MemoryMode::Copy,
                                            .lockdown = proto::LockdownState::Unsandboxed };

/// The callable exports an honest host names for PeekModule: `load` and `ping-then-load`.
constexpr auto PeekCallables = std::array { uint32_t { 1 }, uint32_t { 2 } };

/// What a scripted helper answers at each step: the frame it sends, built from the id the parent chose.
struct HelperScript
{
    /// Answers Load.
    std::function<std::vector<uint8_t>(uint32_t moduleId)> loaded = [](uint32_t moduleId) {
        return frameOf(peekDescription(moduleId));
    };
    /// Answers Instantiate.
    std::function<std::vector<uint8_t>(uint32_t instanceId)> instantiated = [](uint32_t instanceId) {
        return frameOf(
            proto::Instantiated { .instanceId = instanceId,
                                  .error = std::nullopt,
                                  .callableExports = { PeekCallables.begin(), PeekCallables.end() } });
    };
    std::vector<uint8_t> laneBytes {}; ///< Written on every lane once the instance is answered for.
};

/// Plays a helper that says Hello, creates the runtime and answers as @p script says, until the parent closes
/// its ends.
void serve(HelperEnds& ends, HelperScript const& script)
{
    auto const served = [&] {
        if (!ends.send(HonestHello))
            return false;
        auto const create = ends.receive<proto::CreateRuntime>();
        if (!create
            || !ends.send(proto::RuntimeCreated { .runtimeId = create->runtimeId, .error = std::nullopt }))
            return false;
        auto const load = ends.receive<proto::Load>();
        if (!load || !ends.sendFrame(script.loaded(load->moduleId)))
            return false;
        auto const instantiate = ends.receive<proto::Instantiate>();
        return instantiate && ends.sendFrame(script.instantiated(instantiate->instanceId));
    }();
    if (served && !script.laneBytes.empty())
        ends.writeOnEveryLane(script.laneBytes);
    ends.drain();
}

/// @return A connector whose every host plays @p script.
[[nodiscard]] std::shared_ptr<ScriptedConnector> scriptedConnector(HelperScript script)
{
    return std::make_shared<ScriptedConnector>(
        [script = std::move(script)](HelperEnds& ends, std::size_t /*connection*/) { serve(ends, script); });
}

/// A runtime of a remote backend whose hosts a connector the test holds starts.
struct ScriptedRig
{
    std::shared_ptr<ScriptedConnector> connector;
    std::unique_ptr<IWasmBackend> backend = makeBackend(connector);
    std::shared_ptr<IWasmRuntime> runtime = contract::createRuntime(*backend);
};

/// @return A PeekModule instance, served by a host playing @p script, which must succeed.
struct ScriptedInstance
{
    explicit ScriptedInstance(HelperScript script): rig { .connector = scriptedConnector(std::move(script)) }
    {
    }

    ScriptedRig rig;
    std::unique_ptr<vtextension::wasm::IThreadGuard> guard = contract::enterThread(*rig.backend);
    std::shared_ptr<IWasmModule> module = contract::load(*rig.runtime, PeekModule);
    contract::RecordingHostContext context;
    std::unique_ptr<IWasmInstance> instance = contract::instantiate(*module, context);
};

/// @return What calling `load(0)` of @p instance fails with; nothing if it returned.
[[nodiscard]] std::optional<CallFailure> loadFailure(IWasmInstance& instance)
{
    auto argv = std::array<uint32_t, 1> { 0 };
    auto const called = instance.call(contract::functionOf(instance, "load"), argv);
    return called ? std::nullopt : std::optional { called.error() };
}

/// Loads PeekModule through a scripted host that opens with @p hello, driven as @p lockdown asks. The host
/// answers whatever it is asked, so that a parent that wrongly accepted it gets as far as a loaded module --
/// and the case fails -- rather than waiting for an answer forever.
/// @return What the load failed with; nothing if it succeeded.
[[nodiscard]] std::optional<ExtensionError> loadAfterHello(
    proto::Hello hello,
    int& terminations,
    LockdownRequirement lockdown = LockdownRequirement::AcceptUnsandboxed)
{
    auto const connector =
        std::make_shared<ScriptedConnector>([hello](HelperEnds& ends, std::size_t /*connection*/) {
            auto const created = [&] {
                if (!ends.send(hello))
                    return false;
                auto const create = ends.receive<proto::CreateRuntime>();
                return create
                       && ends.send(
                           proto::RuntimeCreated { .runtimeId = create->runtimeId, .error = std::nullopt });
            }();
            if (created)
            {
                if (auto const load = ends.receive<proto::Load>())
                    std::ignore = ends.sendFrame(frameOf(peekDescription(load->moduleId)));
            }
            ends.drain();
        });
    {
        auto const backend = makeBackend(connector, lockdown);
        auto const runtime = contract::createRuntime(*backend);
        auto const loaded = runtime->load(ModuleIdentity {}, toModule(PeekModule));
        terminations = connector->terminations();
        if (!loaded)
            return loaded.error();
    }
    return std::nullopt;
}

/// Plays a helper that serves up to instantiation, then writes @p bytes on every lane; checks that the
/// parent's call reading them fails `HostCrashed`, and that the host was terminated exactly once.
void checkLaneBytesKillTheHelper(std::span<uint8_t const> bytes)
{
    auto scripted = ScriptedInstance { HelperScript { .laneBytes = { bytes.begin(), bytes.end() } } };
    auto& instance = *scripted.instance;
    CHECK(loadFailure(instance) == CallFailure::HostCrashed);
    CHECK(scripted.rig.connector->terminations() == 1);
    CHECK(loadFailure(instance) == CallFailure::HostCrashed);
    CHECK(instance.memory(0, 4).error() == MemoryFailure::HostCrashed);
    CHECK(scripted.rig.connector->terminations() == 1);
}

} // namespace

CONTOUR_WASM_BACKEND_CONTRACT(RemoteTraits)

TEST_CASE("RemoteBackend.sameIdentitySharesAHelper", "[vtextension][remote]")
{
    auto const connector = makeConnector();
    auto const backend = makeBackend(connector);
    auto const runtime = contract::createRuntime(*backend);

    auto const first = loadAs(*runtime, "a", contract::InstanceModule);
    auto const second = loadAs(*runtime, "a", contract::InstanceModule);
    CHECK(connector->connects() == 1);
    auto const third = loadAs(*runtime, "b", contract::InstanceModule);
    CHECK(connector->connects() == 2);
    auto const fourth = loadAs(*runtime, "", contract::InstanceModule);
    auto const fifth = loadAs(*runtime, "", contract::InstanceModule);
    CHECK(connector->connects() == 4);
    CHECK(connector->liveHosts() == 4);
}

TEST_CASE("RemoteBackend.refusesAStackNoHostRuns", "[vtextension][remote]")
{
    // Refused before any host is started: a host would refuse it too, and be started and stopped for nothing
    // on every load.
    auto const connector = makeConnector();
    auto const backend = makeBackend(connector);
    auto limits = contract::Limits;
    limits.stackBytes = proto::MaxRuntimeStackBytes + 1;
    auto const refused = backend->createRuntime(limits, contract::HostFunctions);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == ExtensionError::RuntimeUnavailable);
    CHECK(connector->connects() == 0);

    limits.stackBytes = proto::MaxRuntimeStackBytes;
    auto const runtime = contract::createRuntime(*backend, limits);
    auto const module = loadAs(*runtime, "", contract::InstanceModule);
    CHECK(connector->connects() == 1);
}

TEST_CASE("RemoteBackend.helperDeathFailsCallsHostCrashed", "[vtextension][remote]")
{
    auto const connector = makeConnector();
    auto const backend = makeBackend(connector);
    auto const runtime = contract::createRuntime(*backend);
    auto const guard = contract::enterThread(*backend);
    auto const module = contract::load(*runtime, contract::InstanceModule);
    auto context = contract::RecordingHostContext {};
    auto const instance = contract::instantiate(*module, context);
    REQUIRE(addFailure(*instance) == std::nullopt);

    CHECK(connector->terminateAll() == 1);

    CHECK(addFailure(*instance) == CallFailure::HostCrashed);
    CHECK(instance->memory(0, 4).error() == MemoryFailure::HostCrashed);
    CHECK(addFailure(*instance) == CallFailure::HostCrashed);
    CHECK(connector->connects() == 1);
}

TEST_CASE("RemoteBackend.instantiateRespawnsLazily", "[vtextension][remote]")
{
    auto const connector = makeConnector();
    auto const backend = makeBackend(connector);
    auto const runtime = contract::createRuntime(*backend);
    auto const guard = contract::enterThread(*backend);
    auto const module = contract::load(*runtime, contract::InstanceModule);
    auto context = contract::RecordingHostContext {};
    auto const dead = contract::instantiate(*module, context);
    REQUIRE(connector->terminateAll() == 1);
    REQUIRE(addFailure(*dead) == CallFailure::HostCrashed);
    CHECK(connector->connects() == 1);

    auto const respawned = contract::instantiate(*module, context);
    CHECK(connector->connects() == 2);
    CHECK(addFailure(*respawned) == std::nullopt);
    // The instance of the dead host stays dead.
    CHECK(addFailure(*dead) == CallFailure::HostCrashed);
}

TEST_CASE("RemoteBackend.writesFlushBeforeCall", "[vtextension][remote]")
{
    auto const backend = makeBackend(makeConnector());
    auto const runtime = contract::createRuntime(*backend);
    auto const guard = contract::enterThread(*backend);
    auto const module = contract::load(*runtime, PeekModule);
    auto context = contract::RecordingHostContext {};
    auto const instance = contract::instantiate(*module, context);

    constexpr auto Address = uint32_t { 128 };
    writeAt(*instance, Address, std::to_array<uint8_t>({ 1, 2, 3, 4 }));
    CHECK(callWord(*instance, "load", Address) == 0x0403'0201);
    // Read back from the instance itself, the cache being dropped by the call.
    CHECK(contract::wordAt(*instance, Address) == 0x0403'0201);
}

TEST_CASE("RemoteBackend.writesFlushBeforeHostResult", "[vtextension][remote]")
{
    // The host function writes to the guest's memory; the guest reads it once the host function returned.
    auto const backend = makeBackend(makeConnector());
    auto const runtime = contract::createRuntime(*backend);
    auto const guard = contract::enterThread(*backend);
    auto const module = contract::load(*runtime, PeekModule);
    auto context = WritingHostContext {};
    auto const instance = contract::instantiate(*module, context);
    context.attach(*instance);

    CHECK(callWord(*instance, "ping-then-load", WritingHostContext::Address) == 0x0607'0809);
}

TEST_CASE("RemoteBackend.overlappingViewsSeePendingWrites", "[vtextension][remote]")
{
    auto const backend = makeBackend(makeConnector());
    auto const runtime = contract::createRuntime(*backend);
    auto const guard = contract::enterThread(*backend);
    auto const module = contract::load(*runtime, PeekModule);
    auto context = contract::RecordingHostContext {};
    auto const instance = contract::instantiate(*module, context);

    constexpr auto Address = uint32_t { 256 };
    writeAt(*instance, Address, std::to_array<uint8_t>({ 1, 2, 3, 4, 5, 6, 7, 8 }));
    auto const later = instance->memory(Address + 4, 8);
    REQUIRE(later.has_value());
    CHECK(std::ranges::equal(
        later->first(4),
        std::to_array({ std::byte { 5 }, std::byte { 6 }, std::byte { 7 }, std::byte { 8 } })));
    std::ranges::copy(
        std::to_array({ std::byte { 9 }, std::byte { 10 }, std::byte { 11 }, std::byte { 12 } }),
        later->begin() + 4);

    CHECK(callWord(*instance, "load", Address) == 0x0403'0201);
    CHECK(callWord(*instance, "load", Address + 4) == 0x0807'0605);
    CHECK(callWord(*instance, "load", Address + 8) == 0x0C0B'0A09);
}

TEST_CASE("RemoteBackend.versionMismatchIsHostUnavailable", "[vtextension][remote]")
{
    auto hello = HonestHello;
    hello.version = proto::ProtocolVersion + 1;
    auto terminations = 0;
    CHECK(loadAfterHello(hello, terminations) == ExtensionError::HostUnavailable);
    CHECK(terminations == 1);
}

TEST_CASE("RemoteBackend.lockdownFailureIsHostUnavailable", "[vtextension][remote]")
{
    auto hello = HonestHello;
    hello.lockdown = proto::LockdownState::Failed;
    auto terminations = 0;
    CHECK(loadAfterHello(hello, terminations) == ExtensionError::HostUnavailable);
    CHECK(terminations == 1);
}

TEST_CASE("RemoteBackend.unsandboxedIsRefusedUnlessAccepted", "[vtextension][remote]")
{
    auto terminations = 0;
    CHECK(loadAfterHello(HonestHello, terminations, LockdownRequirement::RequireLockedDown)
          == ExtensionError::HostUnavailable);
    CHECK(terminations == 1);

    // The same host is driven once it accepts hosts without a sandbox, and one in its sandbox under either.
    CHECK(loadAfterHello(HonestHello, terminations, LockdownRequirement::AcceptUnsandboxed) == std::nullopt);
    CHECK(terminations == 0);
    auto lockedDown = HonestHello;
    lockedDown.lockdown = proto::LockdownState::LockedDown;
    CHECK(loadAfterHello(lockedDown, terminations, LockdownRequirement::RequireLockedDown) == std::nullopt);
    CHECK(terminations == 0);
}

TEST_CASE("RemoteBackend.refusedHelloIsLoggedOncePerReason", "[vtextension][remote]")
{
    struct RefusalRow
    {
        proto::Hello hello;
        LockdownRequirement lockdown = LockdownRequirement::AcceptUnsandboxed;
        HostRefusal refusal = HostRefusal::ProtocolVersion;
    };
    auto versioned = HonestHello;
    versioned.version = proto::ProtocolVersion + 1;
    auto failed = HonestHello;
    failed.lockdown = proto::LockdownState::Failed;
    auto shared = HonestHello;
    shared.memoryMode = proto::MemoryMode::Shared;
    auto const rows = std::array {
        RefusalRow { versioned, LockdownRequirement::AcceptUnsandboxed, HostRefusal::ProtocolVersion },
        RefusalRow { failed, LockdownRequirement::AcceptUnsandboxed, HostRefusal::LockdownFailed },
        RefusalRow { HonestHello, LockdownRequirement::RequireLockedDown, HostRefusal::Unsandboxed },
        RefusalRow { shared, LockdownRequirement::AcceptUnsandboxed, HostRefusal::MemoryMode },
    };
    auto const runtime = proto::CreateRuntime {
        .runtimeId = HelperIncarnation::RuntimeId, .memoryBytes = 65536, .stackBytes = 65536, .functions = {}
    };
    for (auto const& row: rows)
    {
        INFO(std::to_underlying(row.refusal));
        auto const connector = std::make_shared<ScriptedConnector>(
            [hello = row.hello](HelperEnds& ends, std::size_t /*connection*/) {
                if (ends.send(hello))
                    ends.drain();
            });
        auto lines = std::vector<std::string> {};
        // One log, as one backend has: the same host refused again says nothing new.
        auto refusals = HostRefusalLog { [&lines](std::string_view line) { lines.emplace_back(line); } };
        for ([[maybe_unused]] auto const attempt: { 1, 2 })
        {
            auto const connected = HelperIncarnation::connect(
                *connector, runtime, HelperPolicy { .timeouts = {}, .lockdown = row.lockdown }, refusals);
            REQUIRE_FALSE(connected.has_value());
            CHECK(connected.error() == ExtensionError::HostUnavailable);
        }
        CHECK(lines
              == std::vector<std::string> {
                  std::string { hostRefusals()[std::to_underlying(row.refusal)].text } });
    }
}

TEST_CASE("RemoteBackend.eachStepIsWatched", "[vtextension][remote]")
{
    auto const connector = scriptedConnector(HelperScript {});
    {
        auto const backend = makeRemoteBackend(
            sharedKind(),
            BackendConfig { .timeouts = { .load = std::chrono::milliseconds { 11 },
                                          .instantiate = std::chrono::milliseconds { 12 },
                                          .handshake = std::chrono::milliseconds { 13 },
                                          .interruptGrace = std::chrono::milliseconds { 14 } },
                            .connector = connector,
                            .lockdown = LockdownRequirement::AcceptUnsandboxed });
        auto const runtime = contract::createRuntime(*backend);
        auto const guard = contract::enterThread(*backend);
        auto const module = contract::load(*runtime, PeekModule);
        auto context = contract::RecordingHostContext {};
        auto const instance = contract::instantiate(*module, context);

        // Hello and CreateRuntime under one handshake watch, then one per exchange: each budget the
        // configuration's, and each watch disarmed once its exchange returned.
        CHECK(connector->watches()
              == std::vector {
                  WatchRecord { .step = HostStep::Handshake, .budget = std::chrono::milliseconds { 13 } },
                  WatchRecord { .step = HostStep::Load, .budget = std::chrono::milliseconds { 11 } },
                  WatchRecord { .step = HostStep::Instantiate, .budget = std::chrono::milliseconds { 12 } },
              });
        CHECK(connector->disarmed() == 3);
    }
}

TEST_CASE("RemoteBackend.observedDeathKillsTheIncarnation", "[vtextension][remote]")
{
    // The handle reports its host's end through the attachment: the incarnation is dead at once, without a
    // request finding out, and the next instantiation starts a host anew.
    auto instance = ScriptedInstance { HelperScript {} };
    auto const onDeath = instance.rig.connector->attachment().onDeath;
    REQUIRE(onDeath);
    CHECK(instance.rig.connector->terminations() == 0);
    onDeath();
    CHECK(instance.rig.connector->terminations() == 1);
    onDeath(); // once dead, for good
    CHECK(instance.rig.connector->terminations() == 1);
    CHECK(loadFailure(*instance.instance) == CallFailure::HostCrashed);
}

TEST_CASE("RemoteBackend.malformedPduKillsTheHelper", "[vtextension][remote]")
{
    // A frame of kind zero, which names no PDU.
    checkLaneBytesKillTheHelper(std::to_array<uint8_t>({ 0x03, 0x00, 0xff, 0xff }));
}

TEST_CASE("RemoteBackend.overCapPduKillsTheHelper", "[vtextension][remote]")
{
    // A MemoryData header declaring a body 17 bytes larger than the largest memory, and nothing after it.
    checkLaneBytesKillTheHelper(
        headerOf(proto::PduKind::MemoryData, proto::PduLimits {}.maxMemoryBytes + 17));
}

TEST_CASE("RemoteBackend.describedMoreThanDeclaredKillsTheHelper", "[vtextension][remote]")
{
    auto rig = ScriptedRig { .connector = scriptedConnector(HelperScript { .loaded = [](uint32_t moduleId) {
                                 auto loaded = peekDescription(moduleId);
                                 loaded.imports.push_back(loaded.imports.front());
                                 return frameOf(loaded);
                             } }) };
    auto const loaded = rig.runtime->load(ModuleIdentity {}, toModule(PeekModule));
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error() == ExtensionError::HostUnavailable);
    CHECK(rig.connector->terminations() == 1);
}

TEST_CASE("RemoteBackend.overCapReplyIsTheHostsFault", "[vtextension][remote]")
{
    // A Loaded larger than any description of a module may be: the host broke the protocol, the module did
    // not.
    auto rig =
        ScriptedRig { .connector = scriptedConnector(HelperScript { .loaded = [](uint32_t /*moduleId*/) {
                          return headerOf(proto::PduKind::Loaded,
                                          (4 * proto::PduLimits {}.maxModuleBytes) + 1);
                      } }) };
    auto const loaded = rig.runtime->load(ModuleIdentity {}, toModule(PeekModule));
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error() == ExtensionError::HostUnavailable);
    CHECK(rig.connector->terminations() == 1);
}

namespace
{

/// @return A Load answer refusing the module with @p error.
[[nodiscard]] std::function<std::vector<uint8_t>(uint32_t)> loadedFailing(ExtensionError error)
{
    return [error](uint32_t moduleId) {
        return frameOf(proto::Loaded { .moduleId = moduleId, .error = error, .imports = {}, .exports = {} });
    };
}

/// @return An Instantiate answer refusing the instance with @p error.
[[nodiscard]] std::function<std::vector<uint8_t>(uint32_t)> instantiatedFailing(ExtensionError error)
{
    return [error](uint32_t instanceId) {
        return frameOf(
            proto::Instantiated { .instanceId = instanceId, .error = error, .callableExports = {} });
    };
}

} // namespace

TEST_CASE("RemoteBackend.honestRefusalsPassThrough", "[vtextension][remote]")
{
    // What a host may refuse a module or an instance with reaches the caller as it said, and the host lives.
    for (auto const error: { ExtensionError::InvalidComponent,
                             ExtensionError::OutOfMemory,
                             ExtensionError::RuntimeUnavailable,
                             ExtensionError::ModuleTooLarge })
    {
        INFO(static_cast<int>(error));
        {
            auto rig = ScriptedRig { .connector =
                                         scriptedConnector(HelperScript { .loaded = loadedFailing(error) }) };
            auto const loaded = rig.runtime->load(ModuleIdentity {}, toModule(PeekModule));
            REQUIRE_FALSE(loaded.has_value());
            CHECK(loaded.error() == error);
            CHECK(rig.connector->terminations() == 0);
        }
        {
            auto rig = ScriptedRig { .connector = scriptedConnector(
                                         HelperScript { .instantiated = instantiatedFailing(error) }) };
            auto const module = contract::load(*rig.runtime, PeekModule);
            auto context = contract::RecordingHostContext {};
            auto const instance = module->instantiate(context);
            REQUIRE_FALSE(instance.has_value());
            CHECK(instance.error() == error);
            CHECK(rig.connector->terminations() == 0);
        }
    }
}

TEST_CASE("RemoteBackend.wrongRepliesKillTheHelper", "[vtextension][remote]")
{
    struct Case
    {
        std::string_view what;
        HelperScript script;
    };
    auto const instantiatedWith = [](std::vector<uint32_t> callable) {
        return [callable = std::move(callable)](uint32_t instanceId) {
            return frameOf(proto::Instantiated {
                .instanceId = instanceId, .error = std::nullopt, .callableExports = callable });
        };
    };
    auto const cases = std::vector<Case> {
        { .what = "Loaded for another module",
          .script =
              HelperScript {
                  .loaded = [](uint32_t moduleId) { return frameOf(peekDescription(moduleId + 1)); } } },
        { .what = "Instantiated for another instance",
          .script =
              HelperScript { .instantiated =
                                 [](uint32_t instanceId) {
                                     return frameOf(proto::Instantiated {
                                         .instanceId = instanceId + 1,
                                         .error = std::nullopt,
                                         .callableExports = { PeekCallables.begin(), PeekCallables.end() } });
                                 } } },
        { .what = "a callable ordinal past the exports",
          .script = HelperScript { .instantiated = instantiatedWith({ 1, 3 }) } },
        { .what = "a callable memory export",
          .script = HelperScript { .instantiated = instantiatedWith({ 0 }) } },
        { .what = "Loaded failing with what no load fails with",
          .script = HelperScript { .loaded = loadedFailing(ExtensionError::HostUnavailable) } },
        { .what = "Loaded failing as a guest trap",
          .script = HelperScript { .loaded = loadedFailing(ExtensionError::Trapped) } },
        { .what = "Instantiated failing with what no instantiation fails with",
          .script = HelperScript { .instantiated = instantiatedFailing(ExtensionError::HostUnavailable) } },
        { .what = "Instantiated failing as a deadline",
          .script = HelperScript { .instantiated = instantiatedFailing(ExtensionError::DeadlineExceeded) } },
    };
    for (auto const& [what, script]: cases)
    {
        INFO(what);
        auto rig = ScriptedRig { .connector = scriptedConnector(script) };
        auto const module = rig.runtime->load(ModuleIdentity {}, toModule(PeekModule));
        auto context = contract::RecordingHostContext {};
        auto const failure =
            module ? module.value()->instantiate(context).error_or(ExtensionError::Trapped) : module.error();
        CHECK(failure == ExtensionError::HostUnavailable);
        CHECK(rig.connector->terminations() == 1);
    }
}

TEST_CASE("RemoteBackend.memoryDataOfTheWrongSizeKillsTheHelper", "[vtextension][remote]")
{
    // Three bytes, where four were asked for.
    auto scripted = ScriptedInstance { HelperScript {
        .laneBytes =
            frameOf(proto::MemoryData { .status = proto::MemoryStatus::Ok,
                                        .bytes = { std::byte { 1 }, std::byte { 2 }, std::byte { 3 } } }) } };
    CHECK(scripted.instance->memory(0, 4).error() == MemoryFailure::HostCrashed);
    // Checked before calling: had the host survived, the call would wait for an answer it never sends.
    REQUIRE(scripted.rig.connector->terminations() == 1);
    CHECK(loadFailure(*scripted.instance) == CallFailure::HostCrashed);
}

TEST_CASE("RemoteBackend.wrongResultCountKillsTheHelper", "[vtextension][remote]")
{
    // Two results, where `load` returns one.
    auto scripted = ScriptedInstance { HelperScript {
        .laneBytes = frameOf(proto::CallResult { .failure = std::nullopt, .argv = { 1, 2 } }) } };
    CHECK(loadFailure(*scripted.instance) == CallFailure::HostCrashed);
    CHECK(scripted.rig.connector->terminations() == 1);
    CHECK(loadFailure(*scripted.instance) == CallFailure::HostCrashed);
}

TEST_CASE("RemoteBackend.respawnDescribingAnotherModuleIsRefused", "[vtextension][remote]")
{
    // The first host dies after instantiating; every later one describes the module without its last export.
    auto const connector = std::make_shared<ScriptedConnector>([](HelperEnds& ends, std::size_t connection) {
        if (connection == 0)
        {
            serve(ends, HelperScript { .laneBytes = { 0x03, 0x00, 0xff, 0xff } });
            return;
        }
        serve(ends, HelperScript { .loaded = [](uint32_t moduleId) {
                  auto loaded = peekDescription(moduleId);
                  loaded.exports.pop_back();
                  return frameOf(loaded);
              } });
    });
    auto rig = ScriptedRig { .connector = connector };
    auto const guard = contract::enterThread(*rig.backend);
    auto const module = contract::load(*rig.runtime, PeekModule);
    auto context = contract::RecordingHostContext {};
    auto const first = contract::instantiate(*module, context);
    REQUIRE(loadFailure(*first) == CallFailure::HostCrashed);

    for (auto const attempt: std::views::iota(1, 3))
    {
        INFO("attempt " << attempt);
        auto const respawned = module->instantiate(context);
        REQUIRE_FALSE(respawned.has_value());
        CHECK(respawned.error() == ExtensionError::InvalidComponent);
        CHECK(connector->terminations() == 1 + attempt);
    }
    CHECK(module->exports().size() == 3);
}

TEST_CASE("RemoteBackend.fifthLaneThreadFailsItsCall", "[vtextension][remote][threading]")
{
    auto const backend = makeBackend(makeConnector());
    auto const runtime = contract::createRuntime(*backend);
    auto const module = contract::load(*runtime, contract::InstanceModule);
    auto parked = std::latch { static_cast<std::ptrdiff_t>(MaxLaneChannels) };
    auto release = std::promise<void> {};
    auto context = ParkingHostContext { parked, release.get_future().share() };
    auto instances = std::vector<std::unique_ptr<IWasmInstance>> {};
    for ([[maybe_unused]] auto const index: std::views::iota(std::size_t { 0 }, MaxLaneChannels + 1))
        instances.push_back(contract::instantiate(*module, context));

    auto fifth = std::optional<std::optional<CallFailure>> {};
    {
        auto const calls = ParkedCalls { *backend, std::span { instances }.first(MaxLaneChannels) };
        auto releaseOnExit = std::move(release);
        parked.wait();
        fifth = contract::onPreparedThread(*backend, [&] { return addFailure(*instances.back()); });
        releaseOnExit.set_value();
        CHECK(calls.failedGuards() == 0);
    }
    REQUIRE(fifth.has_value());
    CHECK(*fifth == CallFailure::Trap);
    // Every lane is free again.
    CHECK(contract::onPreparedThread(*backend, [&] { return addFailure(*instances.back()); })
          == std::optional<std::optional<CallFailure>> { std::in_place, std::nullopt });
}

TEST_CASE("RemoteBackend.interruptIsWaitFreeOfTheHelper", "[vtextension][remote][threading]")
{
    using namespace std::chrono_literals;

    auto const backend = makeBackend(makeConnector());
    auto const runtime = contract::createRuntime(*backend);
    auto const module = contract::load(*runtime, contract::InstanceModule);
    constexpr auto Parked = std::size_t { 2 };
    auto parked = std::latch { static_cast<std::ptrdiff_t>(Parked) };
    auto release = std::promise<void> {};
    auto context = ParkingHostContext { parked, release.get_future().share() };
    auto instances = std::vector<std::unique_ptr<IWasmInstance>> {};
    for ([[maybe_unused]] auto const index: std::views::iota(std::size_t { 0 }, Parked + 1))
        instances.push_back(contract::instantiate(*module, context));
    auto& spinner = *instances.back();

    auto spun = std::optional<std::expected<void, CallFailure>> {};
    auto interruptTook = std::chrono::steady_clock::duration {};
    {
        auto const calls = ParkedCalls { *backend, std::span { instances }.first(Parked) };
        auto releaseOnExit = std::move(release);
        parked.wait();
        auto spinning = std::jthread { [&] {
            auto const guard = backend->enterThread();
            if (guard)
                spun = spinner.call(*spinner.function("spin"), {});
        } };
        std::this_thread::sleep_for(100ms);
        auto const started = std::chrono::steady_clock::now();
        spinner.interrupt();
        interruptTook = std::chrono::steady_clock::now() - started;
        spinning.join();
        releaseOnExit.set_value();
    }
    CHECK(interruptTook < 1s);
    REQUIRE(spun.has_value());
    CHECK(*spun == std::expected<void, CallFailure> { std::unexpected(CallFailure::Interrupted) });
}

TEST_CASE("RemoteBackend.slotsAreReused", "[vtextension][remote]")
{
    // Nearly twice as many instances as a host has slots, one after the other: each gives its slot back.
    auto const connector = makeConnector();
    auto const backend = makeBackend(connector);
    auto const runtime = contract::createRuntime(*backend);
    auto const guard = contract::enterThread(*backend);
    auto const module = contract::load(*runtime, contract::InstanceModule);
    auto context = contract::RecordingHostContext {};
    auto failures = std::size_t { 0 };
    for ([[maybe_unused]] auto const index: std::views::iota(0, 2000))
        if (!module->instantiate(context))
            ++failures;
    CHECK(failures == 0);
    CHECK(connector->connects() == 1);
    auto const instance = contract::instantiate(*module, context);
    auto argv = std::array<uint32_t, 2> { 2, 3 };
    REQUIRE(instance->call(contract::functionOf(*instance, "add"), argv).has_value());
    CHECK(argv[0] == 5);
}

TEST_CASE("RemoteBackend.slotExhaustionIsRuntimeUnavailable", "[vtextension][remote]")
{
    auto const connector = makeConnector();
    auto const backend = makeBackend(connector);
    auto const runtime = contract::createRuntime(*backend);
    auto const guard = contract::enterThread(*backend);
    auto const module = contract::load(*runtime, contract::InstanceModule);
    auto context = contract::RecordingHostContext {};
    auto instances = std::vector<std::unique_ptr<IWasmInstance>> {};
    for ([[maybe_unused]] auto const index:
         std::views::iota(uint32_t { 0 }, vtextension::wasm::remote::transport::InterruptSlotCount))
    {
        auto instance = module->instantiate(context);
        REQUIRE(instance.has_value());
        instances.push_back(*std::move(instance));
    }

    // Every slot is held: the host takes no more instances, and is no worse for it.
    auto const refused = module->instantiate(context);
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == ExtensionError::RuntimeUnavailable);
    auto argv = std::array<uint32_t, 2> { 2, 3 };
    REQUIRE(instances.front()->call(contract::functionOf(*instances.front(), "add"), argv).has_value());
    CHECK(argv[0] == 5);
    CHECK(connector->connects() == 1);

    // One let go, its slot is another's.
    instances.pop_back();
    CHECK(module->instantiate(context).has_value());
}

TEST_CASE("RemoteBackend.releasedSlotIsLowered", "[vtextension][remote][threading]")
{
    // An instance interrupted, then let go: the next to hold its slot is not interrupted by it, at the next
    // wake of the host's listener.
    using namespace std::chrono_literals;
    auto const backend = makeBackend(makeConnector());
    auto const runtime = contract::createRuntime(*backend);
    auto const guard = contract::enterThread(*backend);
    auto const module = contract::load(*runtime, contract::InstanceModule);
    auto context = contract::RecordingHostContext {};
    {
        auto const interrupted = contract::instantiate(*module, context);
        interrupted->interrupt();
    }
    // Slots are handed out round the page: every other one is taken before the released one comes back.
    auto instances = std::vector<std::unique_ptr<IWasmInstance>> {};
    for ([[maybe_unused]] auto const index:
         std::views::iota(uint32_t { 0 }, vtextension::wasm::remote::transport::InterruptSlotCount))
        instances.push_back(contract::instantiate(*module, context));
    auto& heir = *instances.back();

    // A wake: interrupting a spinning instance has the listener scan the whole page.
    auto& spinner = *instances.front();
    auto spun = std::async(std::launch::async, [&] {
        auto const spinGuard = backend->enterThread();
        return spinGuard ? spinner.call(contract::functionOf(spinner, "spin"), {})
                         : std::expected<void, CallFailure> { std::unexpected(CallFailure::Trap) };
    });
    std::this_thread::sleep_for(100ms);
    spinner.interrupt();
    if (spun.wait_for(5s) != std::future_status::ready)
    {
        // The future's destructor would wait for the spinning call forever: the run ends here, rather than
        // hang.
        FAIL_CHECK("the interrupted call did not return");
        std::fputs("RemoteBackend.releasedSlotIsLowered: the interrupted call did not return\n", stderr);
        std::abort();
    }
    CHECK(spun.get() == std::expected<void, CallFailure> { std::unexpected(CallFailure::Interrupted) });

    auto argv = std::array<uint32_t, 2> { 2, 3 };
    CHECK(heir.call(contract::functionOf(heir, "add"), argv).has_value());
    CHECK(argv[0] == 5);
}

namespace
{
/// @return @p future's value, once ready within five seconds; else the run ends, rather than wait forever on
///         a thread a regression keeps from returning.
template <typename T>
[[nodiscard]] T within(std::future<T>& future, char const* what)
{
    if (future.wait_for(std::chrono::seconds { 5 }) != std::future_status::ready)
    {
        FAIL_CHECK(what);
        std::fputs(what, stderr);
        std::fputs("\n", stderr);
        std::abort();
    }
    return future.get();
}
} // namespace

TEST_CASE("RemoteBackend.readAfterAnInterruptIsArmed", "[vtextension][remote][threading]")
{
    // A deadline ran out between a call and the reads of its results: the read that follows waits on a host
    // that never answers it, and must be the backstop's to end.
    using namespace std::chrono_literals;
    auto scripted = ScriptedInstance { HelperScript {} }; // reads none of its lanes
    scripted.instance->interrupt();
    auto read = std::async(std::launch::async, [&]() -> std::optional<MemoryFailure> {
        auto const guard = scripted.rig.backend->enterThread();
        if (!guard)
            return MemoryFailure::OutOfBounds;
        auto const viewed = scripted.instance->memory(0, 4);
        return viewed ? std::nullopt : std::optional { viewed.error() };
    });

    auto const attachment = scripted.rig.connector->attachment();
    REQUIRE(attachment.lanes != nullptr);
    auto const isArmed = [&] {
        return std::ranges::any_of(attachment.lanes->lanes(), [](LaneWatch const& lane) {
            return lane.observe().arming == LaneArming::Armed;
        });
    };
    auto const deadline = std::chrono::steady_clock::now() + 5s;
    while (!isArmed() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    auto const wasArmed = isArmed();
    // What the backstop's kill does to the parent once the grace ran out.
    attachment.onDeath();
    CHECK(wasArmed);
    CHECK(within(read, "the stalled read did not end with its host") == MemoryFailure::HostCrashed);
}

TEST_CASE("RemoteBackend.interruptedInstancesHostCallIsRefused", "[vtextension][remote][threading]")
{
    // A host that runs on past an interrupt and calls the parent gets a refusal, not a served host call whose
    // time would pause its grace.
    using namespace std::chrono_literals;
    auto arrived = std::make_shared<std::promise<void>>();
    auto go = std::make_shared<std::promise<void>>();
    auto answered = std::make_shared<std::promise<std::optional<ExtensionError>>>();
    auto const connector = std::make_shared<ScriptedConnector>([arrived,
                                                                proceed = go->get_future().share(),
                                                                answered](HelperEnds& ends,
                                                                          std::size_t /*connection*/) {
        auto const script = HelperScript {};
        auto const create = ends.send(HonestHello) ? ends.receive<proto::CreateRuntime>() : std::nullopt;
        if (!create
            || !ends.send(proto::RuntimeCreated { .runtimeId = create->runtimeId, .error = std::nullopt }))
            return;
        auto const load = ends.receive<proto::Load>();
        if (!load || !ends.sendFrame(script.loaded(load->moduleId)))
            return;
        auto const instantiate = ends.receive<proto::Instantiate>();
        if (!instantiate || !ends.sendFrame(script.instantiated(instantiate->instanceId)))
            return;
        auto lane = PduStream { std::move(ends.lanes.front()),
                                proto::ChannelKind::Lane,
                                proto::Direction::ToHelper,
                                proto::PduLimits {} };
        if (!lane.receive())
            return;
        arrived->set_value();
        // The guest runs on, interrupted, and calls `ping(7)`.
        if (proceed.wait_for(5s) != std::future_status::ready
            || !lane.send(proto::CallHost { .functionIndex = 0, .arguments = { 7 } }))
            return;
        auto const answer = lane.receive();
        auto const* const result = answer ? std::get_if<proto::HostResult>(&*answer) : nullptr;
        answered->set_value(result ? result->error : std::nullopt);
        std::ignore = lane.send(proto::CallResult { .failure = CallFailure::HostFailure, .argv = {} });
        ends.drain();
    });
    auto rig = ScriptedRig { .connector = connector };
    auto const guard = contract::enterThread(*rig.backend);
    auto const module = contract::load(*rig.runtime, PeekModule);
    auto context = contract::RecordingHostContext {};
    auto const instance = contract::instantiate(*module, context);

    auto called = std::async(std::launch::async, [&] {
        auto const callGuard = rig.backend->enterThread();
        auto argv = std::array<uint32_t, 1> { 0 };
        return callGuard ? instance->call(contract::functionOf(*instance, "ping-then-load"), argv)
                         : std::expected<void, CallFailure> { std::unexpected(CallFailure::Trap) };
    });
    auto arrival = arrived->get_future();
    within(arrival, "the call never reached the host");
    instance->interrupt();
    go->set_value();
    auto answer = answered->get_future();
    CHECK(within(answer, "the host call was not answered")
          == std::optional { ExtensionError::DeadlineExceeded });
    CHECK(within(called, "the call did not return")
          == std::expected<void, CallFailure> { std::unexpected(CallFailure::Interrupted) });
    CHECK(context.calls().empty());
}

namespace
{
/// The control PDUs a scripted host received, in order, for a test thread to wait on.
class Transcript
{
  public:
    /// Records @p pdu.
    void record(proto::Pdu pdu)
    {
        {
            auto const lock = std::scoped_lock { _mutex };
            _pdus.push_back(std::move(pdu));
        }
        _recorded.notify_all();
    }

    /// Waits up to five seconds until @p count PDUs were recorded.
    /// @return Every one recorded by then.
    [[nodiscard]] std::vector<proto::Pdu> awaitAtLeast(std::size_t count)
    {
        auto lock = std::unique_lock { _mutex };
        std::ignore =
            _recorded.wait_for(lock, std::chrono::seconds { 5 }, [&] { return _pdus.size() >= count; });
        return _pdus;
    }

  private:
    std::mutex _mutex;
    std::condition_variable _recorded;
    std::vector<proto::Pdu> _pdus;
};

/// @return The index of the first of @p pdus that @p matches; their count if none does.
template <typename Matches>
[[nodiscard]] std::size_t indexOf(std::vector<proto::Pdu> const& pdus, Matches matches)
{
    return static_cast<std::size_t>(std::ranges::find_if(pdus, matches) - pdus.begin());
}

/// @return Whether @p pdu releases instance @p instanceId.
[[nodiscard]] bool releasesInstance(proto::Pdu const& pdu, uint32_t instanceId)
{
    return pdu
           == proto::Pdu { proto::Release { .target = proto::ReleaseTarget::Instance, .id = instanceId } };
}

/// What a gated scripted host does once its gate opened.
enum class AfterGate : uint8_t
{
    Silent,  ///< Nothing more is answered.
    Answers, ///< The gated request is answered, and every one after it.
};

/// Plays an honest host that records every control PDU after the handshake in @p transcript and answers
/// each Load and Instantiate -- but the Instantiate numbered @p gated (1 for the first), which it waits on
/// @p gate for (ten seconds at most), telling @p arrived when it got it; and after which it goes on as
/// @p afterGate says.
void serveGated(HelperEnds& ends,
                Transcript& transcript,
                std::size_t gated,
                std::promise<void>& arrived,
                std::shared_future<void> const& gate,
                AfterGate afterGate)
{
    if (!ends.send(HonestHello))
        return;
    auto const create = ends.receive<proto::CreateRuntime>();
    if (!create
        || !ends.send(proto::RuntimeCreated { .runtimeId = create->runtimeId, .error = std::nullopt }))
        return;
    auto instantiates = std::size_t { 0 };
    while (auto pdu = ends.receivePdu())
    {
        transcript.record(*pdu);
        if (auto const* const load = std::get_if<proto::Load>(&*pdu))
        {
            if (!ends.sendFrame(frameOf(peekDescription(load->moduleId))))
                return;
        }
        else if (auto const* const instantiate = std::get_if<proto::Instantiate>(&*pdu))
        {
            if (++instantiates == gated)
            {
                arrived.set_value();
                std::ignore = gate.wait_for(std::chrono::seconds { 10 });
                if (afterGate == AfterGate::Silent)
                    break;
            }
            if (!ends.sendFrame(HelperScript {}.instantiated(instantiate->instanceId)))
                return;
        }
    }
    ends.drain();
}

/// @return @p future ready within @p bound; else the run ends, rather than wait on a thread a regression
///         keeps from returning.
void readyWithin(std::future<void>& future, std::chrono::milliseconds bound, char const* what)
{
    if (future.wait_for(bound) == std::future_status::ready)
        return;
    FAIL_CHECK(what);
    std::fputs(what, stderr);
    std::fputs("\n", stderr);
    std::abort();
}
} // namespace

TEST_CASE("RemoteBackend.releaseWaitsForTheControlChannel", "[vtextension][remote][threading]")
{
    // A thread's request holds the control channel while the test drops an instance: the drop does not wait
    // for it, and its release goes out right after that request's reply, not with the next request.
    using namespace std::chrono_literals;
    auto transcript = Transcript {};
    auto arrived = std::promise<void> {};
    auto gate = std::promise<void> {};
    auto const connector = std::make_shared<ScriptedConnector>(
        [&, opened = gate.get_future().share()](HelperEnds& ends, std::size_t /*connection*/) {
            serveGated(ends, transcript, 2, arrived, opened, AfterGate::Answers);
        });
    auto const backend = makeBackend(connector);
    auto const runtime = contract::createRuntime(*backend);
    auto const module = contract::load(*runtime, PeekModule);
    auto context = contract::RecordingHostContext {};
    auto older = contract::instantiate(*module, context);

    auto instantiating = std::async(std::launch::async, [&] { return module->instantiate(context); });
    auto arrival = arrived.get_future();
    within(arrival, "the second instantiation never reached the host");
    auto dropping = std::async(std::launch::async, [&] { older.reset(); });
    readyWithin(dropping, 100ms, "dropping an instance waited for another thread's request");
    gate.set_value();
    auto const newer = within(instantiating, "the second instantiation did not return");
    REQUIRE(newer.has_value());

    // Recorded: Load, Instantiate, Instantiate, and the release -- with no further request to carry it.
    auto const pdus = transcript.awaitAtLeast(4);
    REQUIRE(pdus.size() >= 4);
    auto const& first = std::get<proto::Instantiate>(pdus.at(1));
    CHECK(std::holds_alternative<proto::Instantiate>(pdus.at(2)));
    CHECK(releasesInstance(pdus.at(3), first.instanceId));

    auto const third = contract::instantiate(*module, context);
    auto const all = transcript.awaitAtLeast(5);
    CHECK(indexOf(all, [&](auto const& pdu) { return releasesInstance(pdu, first.instanceId); }) == 3);
    CHECK(connector->terminations() == 0);
}

TEST_CASE("RemoteBackend.releaseQueuedWhileARequestEndsIsSent", "[vtextension][remote][threading]")
{
    // An instance dropped in the last moment a request holds the control channel -- after its last send,
    // while its watch is disarmed -- finds the channel taken. The request's thread sends the release once it
    // let go: the extension then idles, and no later request would carry it.
    auto transcript = Transcript {};
    auto arrived = std::promise<void> {};
    auto const connector =
        std::make_shared<ScriptedConnector>([&](HelperEnds& ends, std::size_t /*connection*/) {
            serveGated(ends, transcript, 0, arrived, std::shared_future<void> {}, AfterGate::Answers);
        });
    auto const backend = makeBackend(connector);
    auto const runtime = contract::createRuntime(*backend);
    auto const module = contract::load(*runtime, PeekModule);
    auto context = contract::RecordingHostContext {};
    auto older = contract::instantiate(*module, context);

    // The hook runs from the watch's destructor, where an assertion that throws would terminate: it only
    // records how the drop went, and the case checks that once the request returned. A drop that never
    // returns ends the run, rather than hang in the future's destructor.
    enum class Drop : uint8_t
    {
        NotRun,
        Returned,
    };
    auto drop = Drop::NotRun;
    connector->onNextDisarm([&] {
        auto dropping = std::async(std::launch::async, [&] { older.reset(); });
        if (dropping.wait_for(std::chrono::seconds { 5 }) != std::future_status::ready)
        {
            std::fputs("dropping an instance waited for the channel\n", stderr);
            std::abort();
        }
        drop = Drop::Returned;
    });
    auto const newer = contract::instantiate(*module, context);
    CHECK(drop == Drop::Returned);

    // Recorded: Load, Instantiate, Instantiate, and the release, with nothing more asked of the host.
    auto const pdus = transcript.awaitAtLeast(4);
    REQUIRE(pdus.size() >= 4);
    CHECK(releasesInstance(pdus.at(3), std::get<proto::Instantiate>(pdus.at(1)).instanceId));
}

TEST_CASE("RemoteBackend.releaseOverflowKillsTheHelper", "[vtextension][remote][threading]")
{
    // A host that stops reading its control channel while a request is outstanding: every drop after it is
    // queued, at once, until more pile up than a host that reads would ever let -- and that host is killed.
    using namespace std::chrono_literals;
    auto transcript = Transcript {};
    auto arrived = std::promise<void> {};
    auto gate = std::promise<void> {};
    auto const connector = std::make_shared<ScriptedConnector>(
        [&, opened = gate.get_future().share()](HelperEnds& ends, std::size_t /*connection*/) {
            serveGated(ends, transcript, 1, arrived, opened, AfterGate::Silent);
        });
    auto const backend = makeBackend(connector);
    auto const runtime = contract::createRuntime(*backend);
    // Modules, not instances: no more than InterruptSlotCount instances live at once.
    auto modules = std::vector<std::shared_ptr<IWasmModule>> {};
    for ([[maybe_unused]] auto const index:
         std::views::iota(std::size_t { 0 }, vtextension::wasm::remote::MaxPendingReleases + 2))
        modules.push_back(loadAs(*runtime, "one host", PeekModule));
    REQUIRE(connector->connections() == 1);

    auto context = contract::RecordingHostContext {};
    auto instantiating =
        std::async(std::launch::async, [&, held = modules.front()] { return held->instantiate(context); });
    auto arrival = arrived.get_future();
    within(arrival, "the instantiation never reached the host");

    // On a thread of its own, so that a drop that waited would fail the case rather than hang it.
    auto terminationsBefore = std::vector<int> {};
    auto dropping = std::async(std::launch::async, [&] {
        auto slowestDrop = std::chrono::steady_clock::duration {};
        while (modules.size() > 1)
        {
            terminationsBefore.push_back(connector->terminations());
            auto const started = std::chrono::steady_clock::now();
            modules.pop_back();
            slowestDrop = std::max(slowestDrop, std::chrono::steady_clock::now() - started);
        }
        return slowestDrop;
    });
    auto const slowest = within(dropping, "dropping the modules waited for the host");
    CHECK(slowest < 100ms);
    // Killed by the drop one past the limit, not before.
    CHECK(terminationsBefore.at(vtextension::wasm::remote::MaxPendingReleases) == 0);
    CHECK(connector->terminations() == 1);
    auto const refused = within(instantiating, "the outstanding instantiation did not end with its host");
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == ExtensionError::HostUnavailable);
    gate.set_value();
}

TEST_CASE("RemoteBackend.droppedBackendLetsItsHostsGo", "[vtextension][remote][threading]")
{
    // The owner lets go of everything while another thread's request waits on a host that does not answer:
    // nothing it drops waits, and the backend going lets the host go, which ends that request.
    using namespace std::chrono_literals;
    auto transcript = Transcript {};
    auto arrived = std::promise<void> {};
    auto gate = std::promise<void> {};
    auto const connector = std::make_shared<ScriptedConnector>(
        [&, opened = gate.get_future().share()](HelperEnds& ends, std::size_t /*connection*/) {
            serveGated(ends, transcript, 2, arrived, opened, AfterGate::Silent);
        });
    auto backend = makeBackend(connector);
    auto runtime = contract::createRuntime(*backend);
    auto module = contract::load(*runtime, PeekModule);
    auto context = contract::RecordingHostContext {};
    auto instance = contract::instantiate(*module, context);

    auto instantiating =
        std::async(std::launch::async, [&, held = module] { return held->instantiate(context); });
    auto arrival = arrived.get_future();
    within(arrival, "the second instantiation never reached the host");
    auto const straggler = runtime; // a thread that keeps using the runtime past its backend
    auto dropping = std::async(std::launch::async, [&] {
        instance.reset();
        module.reset();
        runtime.reset();
        backend.reset();
    });
    readyWithin(dropping, 100ms, "dropping the backend waited for its host");
    CHECK(connector->lettings() == 1);
    CHECK(connector->terminations() == 0);
    auto const refused = within(instantiating, "the outstanding instantiation did not end with its host");
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == ExtensionError::HostUnavailable);

    // No host starts for a backend that went.
    auto const late = straggler->load(ModuleIdentity {}, toModule(PeekModule));
    REQUIRE_FALSE(late.has_value());
    CHECK(late.error() == ExtensionError::HostUnavailable);
    CHECK(connector->connections() == 1);
    gate.set_value();
}

TEST_CASE("RemoteBackend.preparedGenerationFollowsTheOutermostGuard", "[vtextension][remote]")
{
    auto const backend = makeBackend(makeConnector());
    auto const runtime = contract::createRuntime(*backend);
    auto const generations = contract::onFreshThread([&] {
        auto const before = vtextension::wasm::remote::preparedGeneration();
        auto const outer = contract::enterThread(*backend);
        auto const during = vtextension::wasm::remote::preparedGeneration();
        {
            auto const inner = contract::enterThread(*backend);
        }
        auto const afterInner = vtextension::wasm::remote::preparedGeneration();
        return std::array { before, during, afterInner };
    });
    CHECK(generations
          == std::array<uint64_t, 3> { 0, backend->runtimeGeneration(), backend->runtimeGeneration() });
    CHECK(vtextension::wasm::remote::preparedGeneration() == 0);
}

TEST_CASE("Remote.measureRender", "[.measure]")
{
    // Reported, not asserted; vtextension_wasmtime_test's twin measures the same engine in this process.
    auto const backend = RemoteTraits::make();
    vtextension::wasm::testing::measureBundledRender(*backend, "Remote");
}

TEST_CASE("Remote.measureRoundTrip", "[.measure]")
{
    // Reported, not asserted.
    auto const backend = RemoteTraits::make();
    vtextension::wasm::testing::measureRoundTrip(*backend, "Remote");
}
