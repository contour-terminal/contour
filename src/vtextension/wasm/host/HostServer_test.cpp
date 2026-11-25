// SPDX-License-Identifier: Apache-2.0
//
// The extension host's server, driven PDU by PDU from a scripted parent over real channels, on a real engine:
// what each request is answered with, on which channel, and how the connection ends.
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <vtextension/ExtensionError.hpp>
#include <vtextension/testing/TestModules.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>
#include <vtextension/wasm/host/ExecutorThread.hpp>
#include <vtextension/wasm/host/HostServer.hpp>
#include <vtextension/wasm/host/IHostedEngine.hpp>
#include <vtextension/wasm/host/InProcessHost.hpp>
#include <vtextension/wasm/remote/HostConnector.hpp>
#include <vtextension/wasm/remote/proto/Pdu.hpp>
#include <vtextension/wasm/remote/transport/Channel.hpp>
#include <vtextension/wasm/remote/transport/ControlPage.hpp>
#include <vtextension/wasm/remote/transport/GuestMemoryMapping.hpp>
#include <vtextension/wasm/remote/transport/HandleGrant.hpp>
#include <vtextension/wasm/remote/transport/OwnedHandle.hpp>
#include <vtextension/wasm/remote/transport/PduStream.hpp>
#include <vtextension/wasm/remote/transport/SharedObject.hpp>
#include <vtextension/wasm/testing/BackendContract.hpp>
#include <vtextension/wasm/testing/DecoratedEngine.hpp>
#include <vtextension/wasm/testing/HostedEngine.hpp>
#include <vtextension/wasm/testing/ProcessModules.hpp>

#ifndef _WIN32
    #include <vtextension/wasm/remote/transport/testing/HandleCopies.hpp>
#endif

#if defined(_MSC_VER) && !defined(__clang__)
    #include <intrin.h>
#endif

using vtextension::ExtensionError;
using vtextension::testing::toModule;
using vtextension::wasm::CallFailure;
using vtextension::wasm::ModuleIdentity;
using vtextension::wasm::host::ExecutorThread;
using vtextension::wasm::host::HostFrameBytes;
using vtextension::wasm::host::HostServer;
using vtextension::wasm::host::HostServerConfig;
using vtextension::wasm::host::InProcessHostConnector;
using vtextension::wasm::host::ServerExit;
using vtextension::wasm::remote::MaxLaneChannels;
using vtextension::wasm::remote::proto::MaxCallDepth;
using vtextension::wasm::remote::proto::MaxRuntimeStackBytes;
using vtextension::wasm::remote::transport::Channel;
using vtextension::wasm::remote::transport::ChannelError;
using vtextension::wasm::remote::transport::createSharedObject;
using vtextension::wasm::remote::transport::grantHandle;
using vtextension::wasm::remote::transport::GuestMemoryMapping;
using vtextension::wasm::remote::transport::HandleGrant;
using vtextension::wasm::remote::transport::HandleTarget;
using vtextension::wasm::remote::transport::InterruptSlotCount;
using vtextension::wasm::remote::transport::makeChannelPair;
using vtextension::wasm::remote::transport::MapAccess;
using vtextension::wasm::remote::transport::MappingError;
using vtextension::wasm::remote::transport::OwnedHandle;
using vtextension::wasm::remote::transport::PduStream;
using vtextension::wasm::remote::transport::SharedMapping;
using vtextension::wasm::remote::transport::SharedObjectUse;
using vtextension::wasm::remote::transport::StreamError;
using vtextension::wasm::testing::makeHostedEngine;
#ifndef _WIN32
using vtextension::wasm::remote::transport::testing::openDescriptorCount;
#endif

namespace proto = vtextension::wasm::remote::proto;
namespace contract = vtextension::wasm::testing::contract;

namespace
{

/// How long a test waits for the server to stop before it fails.
constexpr auto StopDeadline = std::chrono::seconds { 1 };

/// How long an interrupted call may take to return.
constexpr auto InterruptDeadline = std::chrono::seconds { 5 };

/// How lane 0's parent end is held.
enum class LaneZero : uint8_t
{
    Framed, ///< As a PduStream, as every other lane.
    Raw,    ///< As the bare channel, to write what no PduStream would.
};

/// @return A parent end of a control channel.
[[nodiscard]] PduStream parentControl(Channel channel, proto::PduLimits limits)
{
    return PduStream { std::move(channel), proto::ChannelKind::Control, proto::Direction::ToParent, limits };
}

/// @return A parent end of a lane.
[[nodiscard]] PduStream parentLane(Channel channel, proto::PduLimits limits)
{
    return PduStream { std::move(channel), proto::ChannelKind::Lane, proto::Direction::ToParent, limits };
}

/// The parent's side of one HostServer running on a thread of its own, over a Wasmtime engine: the parent
/// ends of every channel, from which the test scripts what the parent sends and checks what it receives.
class ScriptedParent
{
  public:
    explicit ScriptedParent(HostServerConfig config = {}, LaneZero laneZero = LaneZero::Framed):
        ScriptedParent { makeHostedEngine(), std::move(config), laneZero }
    {
    }

    /// @param engine What the server runs modules on.
    /// @param config How the server runs.
    /// @param laneZero How lane 0's parent end is held.
    ScriptedParent(std::unique_ptr<vtextension::wasm::host::IHostedEngine> engine,
                   HostServerConfig config,
                   LaneZero laneZero = LaneZero::Framed):
        _engine { std::move(engine) }
    {
        auto helperLanes = std::vector<Channel> {};
        for ([[maybe_unused]] auto const index: std::views::iota(std::size_t { 0 }, MaxLaneChannels))
        {
            auto pair = makeChannelPair();
            REQUIRE(pair.has_value());
            helperLanes.push_back(std::move(pair->helper));
            if (index == 0 && laneZero == LaneZero::Raw)
                rawLaneZero.emplace(std::move(pair->parent));
            else
                lanes.push_back(parentLane(std::move(pair->parent), config.limits));
        }
        auto controlPair = makeChannelPair();
        REQUIRE(controlPair.has_value());
        control.emplace(parentControl(std::move(controlPair->parent), config.limits));
        server = std::make_unique<HostServer>(
            *_engine, std::move(helperLanes), std::move(controlPair->helper), std::move(config));
        auto task = std::packaged_task<ServerExit()> { [this] { return server->run(); } };
        exit = task.get_future();
        _thread = std::jthread { std::move(task) };
    }

    ScriptedParent(ScriptedParent const&) = delete;
    ScriptedParent(ScriptedParent&&) = delete;
    ScriptedParent& operator=(ScriptedParent const&) = delete;
    ScriptedParent& operator=(ScriptedParent&&) = delete;

    ~ScriptedParent()
    {
        server->stop();
        _thread.join();
    }

    /// @return The next control PDU, which must arrive.
    [[nodiscard]] proto::Pdu receiveControl()
    {
        auto pdu = control->receive();
        REQUIRE(pdu.has_value());
        return std::move(*pdu);
    }

    /// Sends @p pdu on the control channel, which must take it.
    void sendControl(proto::Pdu const& pdu)
    {
        auto const sent = control->send(pdu);
        REQUIRE(sent.has_value());
    }

    /// Receives the Hello the server opens with.
    void skipHello() { REQUIRE(std::holds_alternative<proto::Hello>(receiveControl())); }

    /// Creates runtime @p id with the contract's host functions and limits, which must succeed.
    void createRuntime(uint32_t id)
    {
        auto functions = std::vector<proto::WireHostFunction> {};
        for (auto const& function: contract::HostFunctions)
            functions.push_back(proto::WireHostFunction { .module = std::string { function.module },
                                                          .name = std::string { function.name },
                                                          .parameterCount = function.parameterCount });
        sendControl(proto::CreateRuntime { .runtimeId = id,
                                           .memoryBytes = contract::Limits.memoryBytes,
                                           .stackBytes = contract::Limits.stackBytes,
                                           .tableElements = contract::Limits.tableElements,
                                           .functions = std::move(functions) });
        CHECK(receiveControl()
              == proto::Pdu { proto::RuntimeCreated { .runtimeId = id, .error = std::nullopt } });
    }

    /// @return What loading @p bytes as module @p moduleId on runtime @p runtimeId is answered with.
    [[nodiscard]] proto::Loaded load(uint32_t runtimeId, uint32_t moduleId, std::vector<std::byte> bytes)
    {
        sendControl(proto::Load { .runtimeId = runtimeId, .moduleId = moduleId, .bytes = std::move(bytes) });
        auto answer = receiveControl();
        REQUIRE(std::holds_alternative<proto::Loaded>(answer));
        return std::get<proto::Loaded>(std::move(answer));
    }

    /// Loads @p bytes as module @p moduleId on runtime @p runtimeId, which must succeed.
    /// @return The ordinal of each of its exports, by name.
    [[nodiscard]] proto::Loaded loadOk(uint32_t runtimeId, uint32_t moduleId, std::vector<std::byte> bytes)
    {
        auto loaded = load(runtimeId, moduleId, std::move(bytes));
        REQUIRE(loaded.moduleId == moduleId);
        REQUIRE(!loaded.error.has_value());
        return loaded;
    }

    /// @return What instantiating module @p moduleId as instance @p instanceId, with interrupt slot
    ///         @p interruptSlot, is answered with.
    [[nodiscard]] proto::Instantiated instantiate(uint32_t moduleId,
                                                  uint32_t instanceId,
                                                  uint32_t interruptSlot)
    {
        sendControl(proto::Instantiate {
            .moduleId = moduleId, .instanceId = instanceId, .interruptSlot = interruptSlot });
        auto answer = receiveControl();
        REQUIRE(std::holds_alternative<proto::Instantiated>(answer));
        return std::get<proto::Instantiated>(std::move(answer));
    }

    /// @return What instantiating module @p moduleId as instance @p instanceId is answered with; its
    /// interrupt
    ///         slot is its id.
    [[nodiscard]] proto::Instantiated instantiate(uint32_t moduleId, uint32_t instanceId)
    {
        return instantiate(moduleId, instanceId, instanceId);
    }

    /// Instantiates module @p moduleId as instance @p instanceId, with interrupt slot @p interruptSlot, which
    /// must succeed.
    void instantiateOk(uint32_t moduleId, uint32_t instanceId, uint32_t interruptSlot)
    {
        auto const instantiated = instantiate(moduleId, instanceId, interruptSlot);
        REQUIRE(instantiated.instanceId == instanceId);
        REQUIRE(!instantiated.error.has_value());
    }

    /// Instantiates module @p moduleId as instance @p instanceId, which must succeed; its interrupt slot is
    /// its id.
    void instantiateOk(uint32_t moduleId, uint32_t instanceId)
    {
        instantiateOk(moduleId, instanceId, instanceId);
    }

    /// Sends @p pdu on lane @p lane, which must take it.
    void send(std::size_t lane, proto::Pdu const& pdu) { REQUIRE(lanes.at(lane).send(pdu).has_value()); }

    /// @return The next PDU on lane @p lane, which must arrive.
    [[nodiscard]] proto::Pdu receive(std::size_t lane)
    {
        auto pdu = lanes.at(lane).receive();
        REQUIRE(pdu.has_value());
        return std::move(*pdu);
    }

    /// @return The next PDU on lane @p lane, which must arrive within InterruptDeadline: a case waiting on an
    ///         interrupt that never comes stops the server instead, which ends the wait, and fails.
    [[nodiscard]] proto::Pdu receiveInterrupted(std::size_t lane)
    {
        auto received = std::async(std::launch::async, [this, lane] { return lanes.at(lane).receive(); });
        if (received.wait_for(InterruptDeadline) != std::future_status::ready)
        {
            FAIL_CHECK("no answer within the deadline");
            server->stop();
        }
        auto pdu = received.get();
        REQUIRE(pdu.has_value());
        return std::move(*pdu);
    }

    /// @return Why the server stopped, once it did within StopDeadline; nothing if it still serves.
    [[nodiscard]] std::optional<ServerExit> exitWithinDeadline()
    {
        if (exit.wait_for(StopDeadline) != std::future_status::ready)
            return std::nullopt;
        return exit.get();
    }

  private:
    std::unique_ptr<vtextension::wasm::host::IHostedEngine> _engine;

  public:
    std::optional<PduStream> control;   ///< The control channel's parent end.
    std::vector<PduStream> lanes;       ///< The lanes' parent ends, lane 0 first unless it is raw.
    std::optional<Channel> rawLaneZero; ///< Lane 0's parent end, under LaneZero::Raw.
    std::unique_ptr<HostServer> server; ///< The server under test.
    std::future<ServerExit> exit;       ///< What its run() returned.

  private:
    std::jthread _thread;
};

/// Where each export call into an instance of a probed engine began on its thread's stack, in call order.
class StackProbe
{
  public:
    /// Records that a call began at @p frame.
    void record(std::uintptr_t frame)
    {
        auto const lock = std::scoped_lock { _mutex };
        _frames.push_back(frame);
    }

    /// @return Where each call began, in the order they began.
    [[nodiscard]] std::vector<std::uintptr_t> frames() const
    {
        auto const lock = std::scoped_lock { _mutex };
        return _frames;
    }

  private:
    mutable std::mutex _mutex;
    std::vector<std::uintptr_t> _frames;
};

/// An instance of a probed engine: records where each call begins, and forwards everything.
class ProbedInstance final: public vtextension::wasm::testing::ForwardingInstance
{
  public:
    ProbedInstance(std::unique_ptr<vtextension::wasm::IWasmInstance> inner,
                   std::shared_ptr<StackProbe> probe):
        ForwardingInstance { std::move(inner) }, _probe { std::move(probe) }
    {
    }

    [[nodiscard]] std::expected<void, CallFailure> call(vtextension::wasm::ExportFunction function,
                                                        std::span<uint32_t> argv) override
    {
        // The frame's own address, not a local's: a sanitizer may keep locals on a stack of its own. MSVC has
        // no __builtin_frame_address; the slot of the frame's return address lies in the frame all the same.
#if defined(_MSC_VER) && !defined(__clang__)
        _probe->record(reinterpret_cast<std::uintptr_t>(_AddressOfReturnAddress()));
#else
        _probe->record(reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0)));
#endif
        return ForwardingInstance::call(function, argv);
    }

  private:
    std::shared_ptr<StackProbe> _probe;
};

/// @return A hosted engine whose instances record where each of their calls begins on its thread's stack.
[[nodiscard]] std::unique_ptr<vtextension::wasm::host::IHostedEngine> makeProbedEngine(
    std::shared_ptr<StackProbe> probe)
{
    return std::make_unique<vtextension::wasm::testing::DecoratedEngine>(
        makeHostedEngine(),
        [probe = std::move(probe)](std::unique_ptr<vtextension::wasm::IWasmInstance> instance)
            -> std::unique_ptr<vtextension::wasm::IWasmInstance> {
            return std::make_unique<ProbedInstance>(std::move(instance), probe);
        });
}

/// @return The ordinal of the export named @p name, which @p loaded must describe.
[[nodiscard]] uint32_t ordinalOf(proto::Loaded const& loaded, std::string_view name)
{
    for (auto const [ordinal, description]: std::views::zip(std::views::iota(uint32_t { 0 }), loaded.exports))
        if (description.name == name)
            return ordinal;
    FAIL("no export named " << name);
    return 0;
}

/// Calls `ping(5)` from its start function, during instantiation.
///
///     (module
///       (import "contract:test/host@0" "ping" (func $ping (param i32)))
///       (func $start (call $ping (i32.const 5)))
///       (start $start))
constexpr auto StartPingModule = std::to_array<uint8_t>({
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x02, 0x60, 0x01, 0x7f, 0x00, 0x60, 0x00,
    0x00, 0x02, 0x1d, 0x01, 0x14, 0x63, 0x6f, 0x6e, 0x74, 0x72, 0x61, 0x63, 0x74, 0x3a, 0x74, 0x65, 0x73,
    0x74, 0x2f, 0x68, 0x6f, 0x73, 0x74, 0x40, 0x30, 0x04, 0x70, 0x69, 0x6e, 0x67, 0x00, 0x00, 0x03, 0x02,
    0x01, 0x01, 0x08, 0x01, 0x01, 0x0a, 0x08, 0x01, 0x06, 0x00, 0x41, 0x05, 0x10, 0x00, 0x0b,
});

/// Exports one function, taking and returning nothing.
///
///     (module (func (export "f")))
constexpr auto OneExportModule = std::to_array<uint8_t>({
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x04, 0x01, 0x60, 0x00, 0x00, 0x03, 0x02,
    0x01, 0x00, 0x07, 0x05, 0x01, 0x01, 0x66, 0x00, 0x00, 0x0a, 0x04, 0x01, 0x02, 0x00, 0x0b,
});

/// Recurses `n` calls deep, then calls `pair(0, 0)`: a host call with `n` guest frames beneath it.
///
///     (module
///       (import "contract:test/host@0" "pair" (func $pair (param i32 i32)))
///       (func $down (export "down") (param i32)
///         (if (local.get 0)
///           (then (call $down (i32.sub (local.get 0) (i32.const 1))))
///           (else (call $pair (i32.const 0) (i32.const 0))))))
constexpr auto DeepModule = std::to_array<uint8_t>({
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x0a, 0x02, 0x60, 0x02, 0x7f, 0x7f,
    0x00, 0x60, 0x01, 0x7f, 0x00, 0x02, 0x1d, 0x01, 0x14, 0x63, 0x6f, 0x6e, 0x74, 0x72, 0x61,
    0x63, 0x74, 0x3a, 0x74, 0x65, 0x73, 0x74, 0x2f, 0x68, 0x6f, 0x73, 0x74, 0x40, 0x30, 0x04,
    0x70, 0x61, 0x69, 0x72, 0x00, 0x00, 0x03, 0x02, 0x01, 0x01, 0x07, 0x08, 0x01, 0x04, 0x64,
    0x6f, 0x77, 0x6e, 0x00, 0x01, 0x0a, 0x17, 0x01, 0x15, 0x00, 0x20, 0x00, 0x04, 0x40, 0x20,
    0x00, 0x41, 0x01, 0x6b, 0x10, 0x01, 0x05, 0x41, 0x00, 0x41, 0x00, 0x10, 0x00, 0x0b, 0x0b,
});

/// Appends @p value to @p out as an unsigned LEB128.
void appendLeb(std::vector<std::byte>& out, uint32_t value)
{
    do
    {
        auto const low = static_cast<uint8_t>(value & 0x7f);
        value >>= 7;
        out.push_back(std::byte { static_cast<uint8_t>(low | (value != 0 ? 0x80 : 0x00)) });
    } while (value != 0);
}

/// The most parameters a function may take, by the parser's limit: each import's description carries them.
constexpr auto WideSignatureParameters = uint32_t { 1000 };

/// @return A module importing @p count functions that each take WideSignatureParameters i32s, from module
///         "m" under three-letter names: a small module whose description is a thousand times larger.
[[nodiscard]] std::vector<std::byte> wideImportsModule(uint32_t count)
{
    auto module = std::vector<std::byte> {};
    for (auto const byte: { 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00 })
        module.push_back(std::byte { static_cast<uint8_t>(byte) });

    auto types = std::vector<std::byte> {};
    appendLeb(types, 1);
    types.push_back(std::byte { 0x60 });
    appendLeb(types, WideSignatureParameters);
    types.insert(types.end(), WideSignatureParameters, std::byte { 0x7f });
    appendLeb(types, 0);

    auto imports = std::vector<std::byte> {};
    appendLeb(imports, count);
    for (auto const index: std::views::iota(uint32_t { 0 }, count))
    {
        appendLeb(imports, 1);
        imports.push_back(std::byte { 'm' });
        appendLeb(imports, 3);
        for (auto const digit: { index / 676, (index / 26) % 26, index % 26 })
            imports.push_back(std::byte { static_cast<uint8_t>('a' + digit) });
        imports.push_back(std::byte { 0x00 }); // a function,
        appendLeb(imports, 0);                 // of the one type
    }

    for (auto const& [id, section]: { std::pair { 1, &types }, std::pair { 2, &imports } })
    {
        module.push_back(std::byte { static_cast<uint8_t>(id) });
        appendLeb(module, static_cast<uint32_t>(section->size()));
        module.insert(module.end(), section->begin(), section->end());
    }
    return module;
}

} // namespace

TEST_CASE("HostServer.helloComesFirst", "[vtextension][host]")
{
    auto parent = ScriptedParent {};
    CHECK(parent.receiveControl()
          == proto::Pdu { proto::Hello { .version = proto::ProtocolVersion,
                                         .memoryMode = proto::MemoryMode::Copy,
                                         .lockdown = proto::LockdownState::Unsandboxed } });
}

TEST_CASE("HostServer.loadDescribes", "[vtextension][host]")
{
    auto parent = ScriptedParent {};
    parent.skipHello();
    parent.createRuntime(1);
    auto const loaded = parent.loadOk(1, 1, toModule(contract::DescribedModule));

    // What the same engine says of the module in this process.
    auto const engine = makeHostedEngine();
    auto const runtime = engine->createRuntime(contract::Limits, contract::HostFunctions);
    REQUIRE(runtime.has_value());
    auto const guard = engine->enterThread();
    REQUIRE(guard.has_value());
    auto const module = (*runtime)->load(ModuleIdentity {}, toModule(contract::DescribedModule));
    REQUIRE(module.has_value());

    CHECK(std::ranges::equal(loaded.imports, (*module)->imports()));
    CHECK(std::ranges::equal(loaded.exports, (*module)->exports()));
    CHECK(loaded.imports.size() == 2);
}

TEST_CASE("HostServer.callRoundTrip", "[vtextension][host]")
{
    auto parent = ScriptedParent {};
    parent.skipHello();
    parent.createRuntime(1);
    auto const loaded = parent.loadOk(1, 1, toModule(contract::InstanceModule));
    auto const instantiated = parent.instantiate(1, 1);
    REQUIRE(!instantiated.error.has_value());
    auto const add = ordinalOf(loaded, "add");
    CHECK(std::ranges::contains(instantiated.callableExports, add));

    parent.send(0, proto::Call { .instanceId = 1, .exportOrdinal = add, .argv = { 2, 3 } });
    CHECK(parent.receive(0) == proto::Pdu { proto::CallResult { .failure = std::nullopt, .argv = { 5 } } });

    // Any lane serves any instance that is not busy on another.
    parent.send(3, proto::Call { .instanceId = 1, .exportOrdinal = add, .argv = { 40, 2 } });
    CHECK(parent.receive(3) == proto::Pdu { proto::CallResult { .failure = std::nullopt, .argv = { 42 } } });
}

TEST_CASE("HostServer.callHostNests", "[vtextension][host]")
{
    auto parent = ScriptedParent {};
    parent.skipHello();
    parent.createRuntime(1);
    auto const pairModule = parent.loadOk(1, 1, toModule(contract::PairModule));
    auto const instanceModule = parent.loadOk(1, 2, toModule(contract::InstanceModule));
    parent.instantiateOk(1, 1);
    parent.instantiateOk(2, 2);

    parent.send(
        0, proto::Call { .instanceId = 1, .exportOrdinal = ordinalOf(pairModule, "pair"), .argv = { 7, 9 } });
    CHECK(parent.receive(0) == proto::Pdu { proto::CallHost { .functionIndex = 1, .arguments = { 7, 9 } } });

    // Parked in the host call, the parent calls in again on the same lane: that call is answered first.
    parent.send(
        0,
        proto::Call { .instanceId = 2, .exportOrdinal = ordinalOf(instanceModule, "add"), .argv = { 2, 3 } });
    CHECK(parent.receive(0) == proto::Pdu { proto::CallResult { .failure = std::nullopt, .argv = { 5 } } });

    parent.send(0, proto::HostResult { .error = std::nullopt });
    CHECK(parent.receive(0) == proto::Pdu { proto::CallResult { .failure = std::nullopt, .argv = {} } });
}

TEST_CASE("HostServer.hostFailureFailsTheCall", "[vtextension][host]")
{
    auto parent = ScriptedParent {};
    parent.skipHello();
    parent.createRuntime(1);
    auto const loaded = parent.loadOk(1, 1, toModule(contract::PairModule));
    parent.instantiateOk(1, 1);

    parent.send(
        1, proto::Call { .instanceId = 1, .exportOrdinal = ordinalOf(loaded, "pair"), .argv = { 1, 2 } });
    CHECK(parent.receive(1) == proto::Pdu { proto::CallHost { .functionIndex = 1, .arguments = { 1, 2 } } });
    parent.send(1, proto::HostResult { .error = ExtensionError::PermissionDenied });
    CHECK(parent.receive(1)
          == proto::Pdu { proto::CallResult { .failure = CallFailure::HostFailure, .argv = {} } });
}

TEST_CASE("HostServer.memoryOpsOnTheLane", "[vtextension][host]")
{
    auto parent = ScriptedParent {};
    parent.skipHello();
    parent.createRuntime(1);
    auto const loaded = parent.loadOk(1, 1, toModule(contract::InstanceModule));
    parent.instantiateOk(1, 1);
    auto const bytes = [](std::string_view text) {
        auto out = std::vector<std::byte> {};
        for (auto const c: text)
            out.push_back(static_cast<std::byte>(c));
        return out;
    };

    SECTION("idle")
    {
        parent.send(0, proto::MemoryWrite { .instanceId = 1, .address = 100, .bytes = bytes("abcd") });
        parent.send(0, proto::MemoryRead { .instanceId = 1, .address = 100, .size = 4 });
        CHECK(parent.receive(0)
              == proto::Pdu {
                  proto::MemoryData { .status = proto::MemoryStatus::Ok, .bytes = bytes("abcd") } });
    }

    SECTION("parked in a host call")
    {
        parent.send(0,
                    proto::Call {
                        .instanceId = 1, .exportOrdinal = ordinalOf(loaded, "ping-then-store"), .argv = {} });
        CHECK(parent.receive(0)
              == proto::Pdu { proto::CallHost { .functionIndex = 0, .arguments = { 42 } } });
        parent.send(0, proto::MemoryWrite { .instanceId = 1, .address = 200, .bytes = bytes("wxyz") });
        parent.send(0, proto::MemoryRead { .instanceId = 1, .address = 200, .size = 4 });
        CHECK(parent.receive(0)
              == proto::Pdu {
                  proto::MemoryData { .status = proto::MemoryStatus::Ok, .bytes = bytes("wxyz") } });
        parent.send(0, proto::HostResult { .error = std::nullopt });
        CHECK(parent.receive(0) == proto::Pdu { proto::CallResult { .failure = std::nullopt, .argv = {} } });

        // What the guest stored after the host call returned.
        parent.send(0, proto::MemoryRead { .instanceId = 1, .address = 0, .size = 4 });
        auto const stored =
            std::vector { std::byte { 1 }, std::byte { 0 }, std::byte { 0 }, std::byte { 0 } };
        CHECK(parent.receive(0)
              == proto::Pdu { proto::MemoryData { .status = proto::MemoryStatus::Ok, .bytes = stored } });
    }

    SECTION("out of bounds")
    {
        parent.send(0, proto::MemoryRead { .instanceId = 1, .address = contract::PageBytes - 2, .size = 4 });
        CHECK(
            parent.receive(0)
            == proto::Pdu { proto::MemoryData { .status = proto::MemoryStatus::OutOfBounds, .bytes = {} } });
    }

    SECTION("a write out of bounds fails the call it belongs to")
    {
        parent.send(0,
                    proto::MemoryWrite {
                        .instanceId = 1, .address = contract::PageBytes - 2, .bytes = bytes("abcd") });
        parent.send(
            0, proto::Call { .instanceId = 1, .exportOrdinal = ordinalOf(loaded, "add"), .argv = { 1, 2 } });
        CHECK(parent.receive(0)
              == proto::Pdu { proto::CallResult { .failure = CallFailure::Trap, .argv = {} } });
        // Only that one.
        parent.send(
            0, proto::Call { .instanceId = 1, .exportOrdinal = ordinalOf(loaded, "add"), .argv = { 1, 2 } });
        CHECK(parent.receive(0)
              == proto::Pdu { proto::CallResult { .failure = std::nullopt, .argv = { 3 } } });
    }

    SECTION("a write out of bounds fails its own instance's call, not a nested one's")
    {
        parent.instantiateOk(1, 2);
        parent.send(0,
                    proto::Call {
                        .instanceId = 1, .exportOrdinal = ordinalOf(loaded, "ping-then-store"), .argv = {} });
        CHECK(parent.receive(0)
              == proto::Pdu { proto::CallHost { .functionIndex = 0, .arguments = { 42 } } });
        parent.send(0,
                    proto::MemoryWrite {
                        .instanceId = 1, .address = contract::PageBytes - 2, .bytes = bytes("abcd") });
        parent.send(
            0, proto::Call { .instanceId = 2, .exportOrdinal = ordinalOf(loaded, "add"), .argv = { 2, 3 } });
        CHECK(parent.receive(0)
              == proto::Pdu { proto::CallResult { .failure = std::nullopt, .argv = { 5 } } });
        parent.send(0, proto::HostResult { .error = std::nullopt });
        CHECK(parent.receive(0)
              == proto::Pdu { proto::CallResult { .failure = CallFailure::HostFailure, .argv = {} } });
    }

    SECTION("an unknown instance is a violation")
    {
        parent.send(0, proto::MemoryRead { .instanceId = 42, .address = 0, .size = 4 });
        CHECK(parent.exitWithinDeadline() == ServerExit::ProtocolViolation);
    }
}

TEST_CASE("HostServer.busyInstanceIsRefusedOnAnotherLane", "[vtextension][host]")
{
    auto parent = ScriptedParent {};
    parent.skipHello();
    parent.createRuntime(1);
    auto const loaded = parent.loadOk(1, 1, toModule(contract::PairModule));
    parent.instantiateOk(1, 1);

    // Lane 0's call is parked in its host call: the instance is lane 0's until that call returns.
    parent.send(
        0, proto::Call { .instanceId = 1, .exportOrdinal = ordinalOf(loaded, "pair"), .argv = { 7, 9 } });
    CHECK(parent.receive(0) == proto::Pdu { proto::CallHost { .functionIndex = 1, .arguments = { 7, 9 } } });

    SECTION("a call")
    {
        parent.send(
            1, proto::Call { .instanceId = 1, .exportOrdinal = ordinalOf(loaded, "pair"), .argv = { 1, 2 } });
    }
    SECTION("a memory read")
    {
        parent.send(1, proto::MemoryRead { .instanceId = 1, .address = 0, .size = 4 });
    }
    CHECK(parent.exitWithinDeadline() == ServerExit::ProtocolViolation);
    CHECK(parent.lanes.at(1).receive() == std::unexpected(StreamError::Closed));
}

TEST_CASE("HostServer.callDepthIsBounded", "[vtextension][host]")
{
    auto parent = ScriptedParent {};
    parent.skipHello();
    parent.createRuntime(1);
    auto const loaded = parent.loadOk(1, 1, toModule(contract::PairModule));
    parent.instantiateOk(1, 1);
    parent.instantiateOk(1, 2);
    auto const pair = ordinalOf(loaded, "pair");

    // MaxCallDepth calls, alternating between two instances, each parked in its host call.
    for (auto const depth: std::views::iota(uint32_t { 0 }, static_cast<uint32_t>(MaxCallDepth)))
    {
        parent.send(
            0, proto::Call { .instanceId = 1 + (depth % 2), .exportOrdinal = pair, .argv = { depth, 0 } });
        CHECK(parent.receive(0)
              == proto::Pdu { proto::CallHost { .functionIndex = 1, .arguments = { depth, 0 } } });
    }

    SECTION("up to the bound, every call returns")
    {
        for ([[maybe_unused]] auto const depth: std::views::iota(std::size_t { 0 }, MaxCallDepth))
        {
            parent.send(0, proto::HostResult { .error = std::nullopt });
            CHECK(parent.receive(0)
                  == proto::Pdu { proto::CallResult { .failure = std::nullopt, .argv = {} } });
        }
    }

    SECTION("one past it is a violation")
    {
        parent.send(0, proto::Call { .instanceId = 1, .exportOrdinal = pair, .argv = { 1, 1 } });
        CHECK(parent.exitWithinDeadline() == ServerExit::ProtocolViolation);
    }
}

TEST_CASE("HostServer.nestedCallsFitTheExecutorStack", "[vtextension][host]")
{
    // Each instance's calls get a stack limit of their own, counted from where the lane's stack stands when
    // they begin: MaxCallDepth calls into as many instances, each nearly filling the largest stack a runtime
    // may give, must fit one executor's stack.
    auto const probe = std::make_shared<StackProbe>();
    auto parent = ScriptedParent { makeProbedEngine(probe), HostServerConfig {} };
    parent.skipHello();
    auto functions = std::vector<proto::WireHostFunction> {};
    for (auto const& function: contract::HostFunctions)
        functions.push_back(proto::WireHostFunction { .module = std::string { function.module },
                                                      .name = std::string { function.name },
                                                      .parameterCount = function.parameterCount });
    parent.sendControl(proto::CreateRuntime { .runtimeId = 1,
                                              .memoryBytes = contract::Limits.memoryBytes,
                                              .stackBytes = MaxRuntimeStackBytes,
                                              .functions = std::move(functions) });
    REQUIRE(parent.receiveControl()
            == proto::Pdu { proto::RuntimeCreated { .runtimeId = 1, .error = std::nullopt } });
    auto const loaded = parent.loadOk(1, 1, toModule(DeepModule));
    auto const down = ordinalOf(loaded, "down");
    for (auto const instance: std::views::iota(uint32_t { 1 }, static_cast<uint32_t>(MaxCallDepth) + 1))
        parent.instantiateOk(1, instance);

    // How deep one call may recurse before its stack runs out: the deepest that reaches the host call.
    auto const reachesTheHost = [&](uint32_t depth) {
        parent.send(1, proto::Call { .instanceId = 1, .exportOrdinal = down, .argv = { depth } });
        auto const answer = parent.receive(1);
        if (std::holds_alternative<proto::CallResult>(answer))
            return false;
        parent.send(1, proto::HostResult { .error = std::nullopt });
        std::ignore = parent.receive(1);
        return true;
    };
    auto shallow = uint32_t { 0 };
    auto deep = uint32_t { 1 } << 22;
    while (deep - shallow > 1)
    {
        auto const middle = shallow + ((deep - shallow) / 2);
        (reachesTheHost(middle) ? shallow : deep) = middle;
    }
    REQUIRE(shallow > 1000);
    // Within a few frames of the limit: the calls take nearly MaxCallDepth whole stacks between them.
    auto const depth = shallow - 64;

    // Nests MaxCallDepth calls on lane 0, each `depth` guest frames deep, and unwinds them.
    // @return What each nesting level took of the lane's stack, from one call's beginning to the next's.
    auto const nestOnLaneZero = [&](uint32_t guestDepth) {
        auto const before = probe->frames().size();
        for (auto const instance: std::views::iota(uint32_t { 1 }, static_cast<uint32_t>(MaxCallDepth) + 1))
        {
            parent.send(
                0, proto::Call { .instanceId = instance, .exportOrdinal = down, .argv = { guestDepth } });
            REQUIRE(parent.receive(0)
                    == proto::Pdu { proto::CallHost { .functionIndex = 1, .arguments = { 0, 0 } } });
        }
        for ([[maybe_unused]] auto const instance: std::views::iota(std::size_t { 0 }, MaxCallDepth))
        {
            parent.send(0, proto::HostResult { .error = std::nullopt });
            CHECK(parent.receive(0)
                  == proto::Pdu { proto::CallResult { .failure = std::nullopt, .argv = {} } });
        }
        auto const frames = probe->frames();
        REQUIRE(frames.size() == before + MaxCallDepth);
        auto const nested = std::span { frames }.subspan(before);
        auto levels = std::vector<std::size_t> {};
        for (auto const [outer, inner]: std::views::zip(nested, nested | std::views::drop(1)))
        {
            REQUIRE(outer > inner); // the stack grows down
            levels.push_back(outer - inner);
        }
        return levels;
    };

    // Nearly whole guest stacks: every level fits what ExecutorStackBytes reserves for it.
    for (auto const levelBytes: nestOnLaneZero(depth))
    {
        CAPTURE(levelBytes);
        CHECK(levelBytes < MaxRuntimeStackBytes + HostFrameBytes);
    }

    // One guest frame each: what is left is the host's share of a level, the engine's entry and exit and the
    // server's frames. It stays within half of HostFrameBytes, so that an engine whose frames grow fails here
    // first, not in a host.
    for (auto const levelBytes: nestOnLaneZero(0))
    {
        CAPTURE(levelBytes);
        CHECK(levelBytes < HostFrameBytes / 2);
    }
}

TEST_CASE("HostServer.refusesAStackTheLanesCannotHold", "[vtextension][host]")
{
    auto parent = ScriptedParent {};
    parent.skipHello();
    parent.sendControl(proto::CreateRuntime { .runtimeId = 1,
                                              .memoryBytes = contract::Limits.memoryBytes,
                                              .stackBytes = MaxRuntimeStackBytes + 1,
                                              .functions = {} });
    CHECK(parent.receiveControl()
          == proto::Pdu {
              proto::RuntimeCreated { .runtimeId = 1, .error = ExtensionError::RuntimeUnavailable } });

    // The largest it holds is accepted.
    parent.sendControl(proto::CreateRuntime { .runtimeId = 2,
                                              .memoryBytes = contract::Limits.memoryBytes,
                                              .stackBytes = MaxRuntimeStackBytes,
                                              .functions = {} });
    CHECK(parent.receiveControl()
          == proto::Pdu { proto::RuntimeCreated { .runtimeId = 2, .error = std::nullopt } });
}

TEST_CASE("HostServer.runtimeHoldsTheParentsTableCap", "[vtextension][host]")
{
    auto parent = ScriptedParent {};
    parent.skipHello();

    // The contract's cap, below the engine's own: a table one element larger is not instantiated.
    parent.createRuntime(1);
    REQUIRE(!parent.load(1, 1, toModule(contract::OversizedTableModule)).error.has_value());
    CHECK(parent.instantiate(1, 1).error == ExtensionError::OutOfMemory);

    // A cap above what every engine accepts is refused, however far above.
    for (auto const [runtimeId, tableElements]: std::to_array<std::pair<uint32_t, uint64_t>>(
             { { 2, uint64_t { vtextension::wasm::MaxTableElements } + 1 },
               { 3, (uint64_t { 1 } << 32) + 64 } }))
    {
        parent.sendControl(proto::CreateRuntime { .runtimeId = runtimeId,
                                                  .memoryBytes = contract::Limits.memoryBytes,
                                                  .stackBytes = contract::Limits.stackBytes,
                                                  .tableElements = tableElements,
                                                  .functions = {} });
        CHECK(parent.receiveControl()
              == proto::Pdu { proto::RuntimeCreated { .runtimeId = runtimeId,
                                                      .error = ExtensionError::RuntimeUnavailable } });
    }
}

TEST_CASE("HostServer.executorThatCannotStartEndsTheServer", "[vtextension][host]")
{
    // The second lane's thread cannot start: the server says why it ends instead of throwing, and the first
    // lane's thread is joined on the way out.
    auto starts = std::make_shared<std::atomic<int>>(0);
    auto config = HostServerConfig {};
    config.startExecutor =
        [starts](std::size_t stackBytes,
                 std::function<void()> body) -> std::expected<ExecutorThread, std::error_code> {
        if (starts->fetch_add(1) == 1)
            return std::unexpected(std::make_error_code(std::errc::resource_unavailable_try_again));
        return ExecutorThread::start(stackBytes, std::move(body));
    };
    auto parent = ScriptedParent { std::move(config) };
    CHECK(parent.exitWithinDeadline() == ServerExit::ExecutorUnavailable);
    // Nothing was sent: a host that cannot serve every lane never says Hello.
    CHECK(parent.control->receive() == std::unexpected(StreamError::Closed));
    CHECK(starts->load() == 2);
}

TEST_CASE("HostServer.confineRunsAfterTheLanesStartAndBeforeHello", "[vtextension][host]")
{
    auto starts = std::make_shared<std::atomic<std::size_t>>(0);
    auto startsAtConfine = std::make_shared<std::atomic<std::size_t>>(0);
    auto config = HostServerConfig {};
    config.startExecutor =
        [starts](std::size_t stackBytes,
                 std::function<void()> body) -> std::expected<ExecutorThread, std::error_code> {
        starts->fetch_add(1);
        return ExecutorThread::start(stackBytes, std::move(body));
    };
    config.confine = [starts, startsAtConfine] {
        startsAtConfine->store(starts->load());
        return proto::LockdownState::Failed;
    };
    auto parent = ScriptedParent { std::move(config) };
    CHECK(parent.receiveControl()
          == proto::Pdu { proto::Hello { .version = proto::ProtocolVersion,
                                         .memoryMode = proto::MemoryMode::Copy,
                                         .lockdown = proto::LockdownState::Failed } });
    CHECK(startsAtConfine->load() == MaxLaneChannels);
}

TEST_CASE("HostServer.noConfineSendsTheConfiguredLockdown", "[vtextension][host]")
{
    auto config = HostServerConfig {};
    config.lockdown = proto::LockdownState::LockedDown;
    auto parent = ScriptedParent { std::move(config) };
    CHECK(parent.receiveControl()
          == proto::Pdu { proto::Hello { .version = proto::ProtocolVersion,
                                         .memoryMode = proto::MemoryMode::Copy,
                                         .lockdown = proto::LockdownState::LockedDown } });
}

TEST_CASE("HostServer.executorFailureSkipsConfine", "[vtextension][host]")
{
    auto confined = std::make_shared<std::atomic<int>>(0);
    auto config = HostServerConfig {};
    config.startExecutor =
        [](std::size_t, std::function<void()> const&) -> std::expected<ExecutorThread, std::error_code> {
        return std::unexpected(std::make_error_code(std::errc::resource_unavailable_try_again));
    };
    config.confine = [confined] {
        confined->fetch_add(1);
        return proto::LockdownState::Unsandboxed;
    };
    auto parent = ScriptedParent { std::move(config) };
    CHECK(parent.exitWithinDeadline() == ServerExit::ExecutorUnavailable);
    CHECK(confined->load() == 0);
}

TEST_CASE("HostServer.malformedFrameEndsTheConnection", "[vtextension][host]")
{
    auto parent = ScriptedParent { HostServerConfig {}, LaneZero::Raw };
    parent.skipHello();

    auto const garbage = std::to_array<uint8_t>({ 0x03, 0x00, 0xff, 0xff });
    REQUIRE(parent.rawLaneZero->send(garbage).has_value());

    CHECK(parent.exitWithinDeadline() == ServerExit::ProtocolViolation);
    CHECK(parent.control->receive() == std::unexpected(StreamError::Closed));
    for (auto& lane: parent.lanes)
        CHECK(lane.receive() == std::unexpected(StreamError::Closed));
    auto buffer = std::array<uint8_t, 16> {};
    CHECK(parent.rawLaneZero->receiveSome(buffer) == std::unexpected(ChannelError::Closed));
}

TEST_CASE("HostServer.peerCloseEndsTheServer", "[vtextension][host]")
{
    auto parent = ScriptedParent {};
    parent.skipHello();
    parent.control.reset();
    CHECK(parent.exitWithinDeadline() == ServerExit::PeerClosed);
    for (auto& lane: parent.lanes)
        CHECK(lane.receive() == std::unexpected(StreamError::Closed));
}

TEST_CASE("HostServer.stopInterruptsARunningGuest", "[vtextension][host]")
{
    auto parent = ScriptedParent {};
    parent.skipHello();
    parent.createRuntime(1);
    auto const loaded = parent.loadOk(1, 1, toModule(contract::InstanceModule));
    parent.instantiateOk(1, 1);

    parent.send(0, proto::Call { .instanceId = 1, .exportOrdinal = ordinalOf(loaded, "spin"), .argv = {} });
    std::this_thread::sleep_for(std::chrono::milliseconds { 100 });
    parent.server->stop();
    CHECK(parent.exitWithinDeadline() == ServerExit::Stopped);
    parent.server->stop(); // idempotent
}

TEST_CASE("HostServer.stopEndsAParkedCall", "[vtextension][host]")
{
    auto parent = ScriptedParent {};
    parent.skipHello();
    parent.createRuntime(1);
    auto const loaded = parent.loadOk(1, 1, toModule(contract::PairModule));
    parent.instantiateOk(1, 1);

    parent.send(
        0, proto::Call { .instanceId = 1, .exportOrdinal = ordinalOf(loaded, "pair"), .argv = { 7, 9 } });
    CHECK(parent.receive(0) == proto::Pdu { proto::CallHost { .functionIndex = 1, .arguments = { 7, 9 } } });
    parent.server->stop();
    CHECK(parent.exitWithinDeadline() == ServerExit::Stopped);
    CHECK(parent.lanes.front().receive() == std::unexpected(StreamError::Closed));
}

TEST_CASE("HostServer.stopBeforeRun", "[vtextension][host]")
{
    auto const engine = makeHostedEngine();
    auto control = makeChannelPair();
    REQUIRE(control.has_value());
    auto server = HostServer { *engine, {}, std::move(control->helper), HostServerConfig {} };
    server.stop();
    CHECK(server.run() == ServerExit::Stopped);
    CHECK(parentControl(std::move(control->parent), proto::PduLimits {}).receive()
          == std::unexpected(StreamError::Closed));
}

TEST_CASE("HostServer.interruptSlotInterruptsItsInstance", "[vtextension][host]")
{
    auto parent = ScriptedParent {};
    parent.skipHello();
    parent.createRuntime(1);
    auto const loaded = parent.loadOk(1, 1, toModule(contract::InstanceModule));
    parent.instantiateOk(1, 1, 5);
    parent.instantiateOk(1, 2, 7);

    parent.send(0, proto::Call { .instanceId = 1, .exportOrdinal = ordinalOf(loaded, "spin"), .argv = {} });
    std::this_thread::sleep_for(std::chrono::milliseconds { 50 });
    parent.server->interruptSlot(6); // no instance holds it: ignored
    parent.server->interruptSlot(5);
    CHECK(parent.receiveInterrupted(0)
          == proto::Pdu { proto::CallResult { .failure = CallFailure::Interrupted, .argv = {} } });

    // The other instance runs on.
    parent.send(1,
                proto::Call { .instanceId = 2, .exportOrdinal = ordinalOf(loaded, "add"), .argv = { 1, 1 } });
    CHECK(parent.receive(1) == proto::Pdu { proto::CallResult { .failure = std::nullopt, .argv = { 2 } } });
}

TEST_CASE("HostServer.slotOutOfRangeIsAViolation", "[vtextension][host]")
{
    auto parent = ScriptedParent {};
    parent.skipHello();
    parent.createRuntime(1);
    std::ignore = parent.loadOk(1, 1, toModule(contract::InstanceModule));
    parent.instantiateOk(1, 1, InterruptSlotCount - 1);
    parent.sendControl(
        proto::Instantiate { .moduleId = 1, .instanceId = 2, .interruptSlot = InterruptSlotCount });
    CHECK(parent.exitWithinDeadline() == ServerExit::ProtocolViolation);
}

TEST_CASE("HostServer.slotInUseIsAViolation", "[vtextension][host]")
{
    auto parent = ScriptedParent {};
    parent.skipHello();
    parent.createRuntime(1);
    std::ignore = parent.loadOk(1, 1, toModule(contract::InstanceModule));
    parent.instantiateOk(1, 1, 3);
    parent.sendControl(proto::Instantiate { .moduleId = 1, .instanceId = 2, .interruptSlot = 3 });
    CHECK(parent.exitWithinDeadline() == ServerExit::ProtocolViolation);
}

TEST_CASE("HostServer.releasedSlotIsHandedOn", "[vtextension][host]")
{
    auto parent = ScriptedParent {};
    parent.skipHello();
    parent.createRuntime(1);
    auto const loaded = parent.loadOk(1, 1, toModule(contract::InstanceModule));
    parent.instantiateOk(1, 1, 3);
    parent.sendControl(proto::Release { .target = proto::ReleaseTarget::Instance, .id = 1 });
    parent.instantiateOk(1, 2, 3);

    // The slot is the new instance's now.
    parent.send(0, proto::Call { .instanceId = 2, .exportOrdinal = ordinalOf(loaded, "spin"), .argv = {} });
    std::this_thread::sleep_for(std::chrono::milliseconds { 50 });
    parent.server->interruptSlot(3);
    CHECK(parent.receiveInterrupted(0)
          == proto::Pdu { proto::CallResult { .failure = CallFailure::Interrupted, .argv = {} } });
}

TEST_CASE("HostServer.slotLoweredSinceIsNotInterrupted", "[vtextension][host]")
{
    // The listener read the slot raised, for an instance whose slot was lowered and handed on meanwhile: the
    // page, asked again, saves the instance holding it now.
    auto raised = std::make_shared<std::atomic<bool>>(false);
    auto config = HostServerConfig {};
    config.isInterruptRaised = [raised](uint32_t slot) {
        return slot == 4 && raised->load();
    };
    auto parent = ScriptedParent { std::move(config) };
    parent.skipHello();
    parent.createRuntime(1);
    auto const loaded = parent.loadOk(1, 1, toModule(contract::InstanceModule));
    parent.instantiateOk(1, 1, 4);
    auto const add =
        proto::Call { .instanceId = 1, .exportOrdinal = ordinalOf(loaded, "add"), .argv = { 2, 3 } };

    parent.server->interruptSlot(4);
    parent.send(0, add);
    CHECK(parent.receive(0) == proto::Pdu { proto::CallResult { .failure = std::nullopt, .argv = { 5 } } });

    raised->store(true);
    parent.server->interruptSlot(4);
    parent.send(0, add);
    CHECK(parent.receive(0)
          == proto::Pdu { proto::CallResult { .failure = CallFailure::Interrupted, .argv = {} } });
}

TEST_CASE("HostServer.releaseDropsTheInstance", "[vtextension][host]")
{
    auto parent = ScriptedParent {};
    parent.skipHello();
    parent.createRuntime(1);
    auto const loaded = parent.loadOk(1, 1, toModule(contract::InstanceModule));
    parent.instantiateOk(1, 1);

    parent.sendControl(proto::Release { .target = proto::ReleaseTarget::Instance, .id = 1 });
    // Release is unanswered: an answered request after it shows it was served before the lane's Call arrives.
    parent.createRuntime(2);

    parent.send(0,
                proto::Call { .instanceId = 1, .exportOrdinal = ordinalOf(loaded, "add"), .argv = { 1, 2 } });
    CHECK(parent.exitWithinDeadline() == ServerExit::ProtocolViolation);
    CHECK(parent.lanes.front().receive() == std::unexpected(StreamError::Closed));
}

TEST_CASE("HostServer.reusedIdIsAViolation", "[vtextension][host]")
{
    auto parent = ScriptedParent {};
    parent.skipHello();
    parent.createRuntime(1);
    std::ignore = parent.loadOk(1, 1, toModule(contract::InstanceModule));
    parent.instantiateOk(1, 1);
    parent.sendControl(proto::Release { .target = proto::ReleaseTarget::Instance, .id = 1 });

    // Released, and so never to be created again.
    parent.sendControl(proto::Instantiate { .moduleId = 1, .instanceId = 1 });
    CHECK(parent.exitWithinDeadline() == ServerExit::ProtocolViolation);
}

TEST_CASE("HostServer.hostCallDuringInstantiateFails", "[vtextension][host]")
{
    auto parent = ScriptedParent {};
    parent.skipHello();
    parent.createRuntime(1);
    std::ignore = parent.loadOk(1, 1, toModule(StartPingModule));
    CHECK(parent.instantiate(1, 1)
          == proto::Instantiated {
              .instanceId = 1, .error = ExtensionError::InvalidComponent, .callableExports = {} });

    // The connection serves on.
    parent.createRuntime(2);
}

TEST_CASE("HostServer.describingPastTheCapFailsTheLoad", "[vtextension][host]")
{
    // A small module whose description is far larger: each of its imports carries a thousand parameters.
    // Under limits that cap a Loaded at 16 KiB, twenty of them do not fit; the load fails, and the stream
    // serves on.
    auto const config =
        HostServerConfig { .limits = proto::PduLimits { .maxModuleBytes = 4096,
                                                        .maxMemoryBytes = uint64_t { 1024 } * 1024 } };
    auto parent = ScriptedParent { config };
    parent.skipHello();
    parent.createRuntime(1);

    auto const module = wideImportsModule(20);
    REQUIRE(module.size() < config.limits.maxModuleBytes);
    CHECK(parent.load(1, 1, module)
          == proto::Loaded {
              .moduleId = 1, .error = ExtensionError::InvalidComponent, .imports = {}, .exports = {} });

    // A module never described is never instantiated: the id is not the server's.
    std::ignore = parent.loadOk(1, 2, toModule(contract::InstanceModule));
    parent.instantiateOk(2, 1);
}

TEST_CASE("HostServer.callablesPastTheCapFailTheInstantiation", "[vtextension][host]")
{
    // Under a module cap of three bytes an Instantiated holds at most three, and naming one callable export
    // takes four; the module itself (31 bytes) still fits the Load's slack, and its description the Loaded's.
    auto const config =
        HostServerConfig { .limits = proto::PduLimits { .maxModuleBytes = 3, .maxMemoryBytes = 1024 } };
    auto parent = ScriptedParent { config };
    parent.skipHello();
    parent.createRuntime(1);
    std::ignore = parent.loadOk(1, 1, toModule(OneExportModule));
    CHECK(parent.instantiate(1, 1)
          == proto::Instantiated {
              .instanceId = 1, .error = ExtensionError::InvalidComponent, .callableExports = {} });

    // The instance went with the refusal: a call of it is not the server's to serve.
    parent.send(0, proto::Call { .instanceId = 1, .exportOrdinal = 0, .argv = {} });
    CHECK(parent.exitWithinDeadline() == ServerExit::ProtocolViolation);
}

namespace
{

/// A guest page.
constexpr auto GuestPage = proto::WasmPageBytes;

/// The object the parent grants each instance of a runtime of the contract's limits.
constexpr auto ObjectBytes = proto::memoryObjectBytes(contract::Limits.memoryBytes);

/// One instance's memory object as the parent holds it: made, and mapped whole, its pages committed.
class ParentObject
{
  public:
    /// @param bytes The object's size.
    explicit ParentObject(uint64_t bytes = ObjectBytes):
        _handle { created(bytes) }, _mapping { mapped(_handle, bytes) }
    {
    }

    /// @return A grant of the object to a host serving in this process.
    [[nodiscard]] HandleGrant grant() const
    {
        auto granted = grantHandle(HandleTarget::currentProcess(), _handle.get());
        REQUIRE(granted.has_value());
        return *granted;
    }

    /// @return The 32-bit little-endian word at @p address, as the parent's mapping reads it.
    [[nodiscard]] uint32_t wordAt(std::size_t address) const
    {
        auto word = uint32_t { 0 };
        for (auto const index: std::views::iota(std::size_t { 0 }, std::size_t { 4 }))
            word |= std::to_integer<uint32_t>(_mapping.bytes()[address + index]) << (index * 8);
        return word;
    }

  private:
    [[nodiscard]] static OwnedHandle created(uint64_t bytes)
    {
        auto object = createSharedObject(bytes, SharedObjectUse::GuestMemory);
        REQUIRE(object.has_value());
        return std::move(*object);
    }

    [[nodiscard]] static SharedMapping mapped(OwnedHandle const& handle, uint64_t bytes)
    {
        auto mapping = SharedMapping::map(handle.get(), bytes, MapAccess::ReadWrite);
        REQUIRE(mapping.has_value());
        // Contour commits what it grants through its own view (Windows); the host does through its own.
        REQUIRE(mapping->commit(0, bytes).has_value());
        return std::move(*mapping);
    }

    OwnedHandle _handle;
    SharedMapping _mapping;
};

/// @return A scripted parent of a host whose engine runs instances in the memory granted to them.
[[nodiscard]] std::unique_ptr<ScriptedParent> sharingParent(HostServerConfig config = {})
{
    return std::make_unique<ScriptedParent>(makeHostedEngine(proto::MemoryMode::Shared), std::move(config));
}

/// Sends an Instantiate of module 1 as instance 1 on @p parent's control, granting what @p grant grants with
/// @p memory's sizes.
void sendGranted(ScriptedParent& parent, proto::MemoryGrant memory, HandleGrant const& grant)
{
    auto const sent = parent.control->send(
        proto::Instantiate { .moduleId = 1, .instanceId = 1, .interruptSlot = 1, .memory = memory }, grant);
    REQUIRE(sent.has_value());
}

/// Instantiates module 1 as instance 1, granting @p object with its first page open, which must succeed.
void instantiateGranted(ScriptedParent& parent, ParentObject const& object)
{
    auto const grant = object.grant();
    sendGranted(parent,
                proto::MemoryGrant {
                    .handle = grant.wireValue, .objectBytes = ObjectBytes, .initialBytes = GuestPage },
                grant);
    auto const answer = parent.receiveControl();
    REQUIRE(std::holds_alternative<proto::Instantiated>(answer));
    REQUIRE_FALSE(std::get<proto::Instantiated>(answer).error.has_value());
}

/// A parent with runtime 1 and GrowModule loaded as module 1 on a host that shares memory.
struct GrowingParent
{
    std::unique_ptr<ScriptedParent> parent; ///< The parent.
    proto::Loaded loaded;                   ///< What GrowModule was described as.

    explicit GrowingParent(HostServerConfig config = {}):
        parent { sharingParent(std::move(config)) }, loaded { prepared(*parent) }
    {
    }

  private:
    [[nodiscard]] static proto::Loaded prepared(ScriptedParent& parent)
    {
        CHECK(parent.receiveControl()
              == proto::Pdu { proto::Hello { .version = proto::ProtocolVersion,
                                             .memoryMode = proto::MemoryMode::Shared,
                                             .lockdown = proto::LockdownState::Unsandboxed } });
        parent.createRuntime(1);
        return parent.loadOk(1, 1, toModule(vtextension::wasm::testing::GrowModule));
    }
};

} // namespace

TEST_CASE("HostServer.sharedInstantiateMapsTheGrant", "[vtextension][host]")
{
    auto growing = GrowingParent {};
    auto& parent = *growing.parent;
    auto const object = ParentObject {};
#ifndef _WIN32
    auto const descriptorsBefore = openDescriptorCount();
#endif
    instantiateGranted(parent, object);
#ifndef _WIN32
    // The host keeps the mapping, not the descriptor it came with.
    CHECK(openDescriptorCount() == descriptorsBefore);
#endif

    parent.send(0,
                proto::Call { .instanceId = 1,
                              .exportOrdinal = ordinalOf(growing.loaded, "store"),
                              .argv = { 16, 42 } });
    CHECK(parent.receive(0) == proto::Pdu { proto::CallResult {} });
    CHECK(object.wordAt(16) == 42);
}

TEST_CASE("HostServer.growGoesThroughTheParent", "[vtextension][host]")
{
    auto growing = GrowingParent {};
    auto& parent = *growing.parent;
    auto const object = ParentObject {};
    instantiateGranted(parent, object);
    auto const grow = ordinalOf(growing.loaded, "grow");

    parent.send(0, proto::Call { .instanceId = 1, .exportOrdinal = grow, .argv = { 1 } });
    CHECK(parent.receive(0) == proto::Pdu { proto::Grow { .instanceId = 1, .bytes = 2 * GuestPage } });

    SECTION("granted")
    {
        parent.send(0, proto::Grown { .status = proto::GrowStatus::Granted });
        CHECK(parent.receive(0)
              == proto::Pdu { proto::CallResult { .failure = std::nullopt, .argv = { 1 } } });
        auto const address = static_cast<uint32_t>(GuestPage + 8);
        parent.send(0,
                    proto::Call { .instanceId = 1,
                                  .exportOrdinal = ordinalOf(growing.loaded, "store"),
                                  .argv = { address, 7 } });
        CHECK(parent.receive(0) == proto::Pdu { proto::CallResult {} });
        CHECK(object.wordAt(address) == 7);
    }

    SECTION("refused")
    {
        parent.send(0, proto::Grown { .status = proto::GrowStatus::Refused });
        CHECK(parent.receive(0)
              == proto::Pdu { proto::CallResult { .failure = std::nullopt, .argv = { 0xFFFF'FFFF } } });
    }
}

TEST_CASE("HostServer.growDuringInstantiateGoesThroughTheParent", "[vtextension][host]")
{
    using vtextension::wasm::testing::StartGrowAddress;
    using vtextension::wasm::testing::StartGrowValue;
    auto parent = sharingParent();
    parent->skipHello();
    parent->createRuntime(1);
    auto const loaded = parent->loadOk(1, 1, toModule(vtextension::wasm::testing::StartGrowModule));
    auto const object = ParentObject {};
    auto const grant = object.grant();
    sendGranted(*parent,
                proto::MemoryGrant {
                    .handle = grant.wireValue, .objectBytes = ObjectBytes, .initialBytes = GuestPage },
                grant);

    // The start function asks on control, before the instance is announced.
    CHECK(parent->receiveControl() == proto::Pdu { proto::Grow { .instanceId = 1, .bytes = 2 * GuestPage } });
    auto const answered = [&](proto::GrowStatus status) {
        parent->sendControl(proto::Grown { .status = status });
        auto const instantiated = parent->receiveControl();
        REQUIRE(std::holds_alternative<proto::Instantiated>(instantiated));
        REQUIRE_FALSE(std::get<proto::Instantiated>(instantiated).error.has_value());
        parent->send(
            0, proto::Call { .instanceId = 1, .exportOrdinal = ordinalOf(loaded, "grown"), .argv = {} });
        auto const result = parent->receive(0);
        REQUIRE(std::holds_alternative<proto::CallResult>(result));
        return std::get<proto::CallResult>(result).argv;
    };

    SECTION("granted")
    {
        CHECK(answered(proto::GrowStatus::Granted) == std::vector<uint32_t> { 1 });
        CHECK(object.wordAt(StartGrowAddress) == StartGrowValue);
    }

    SECTION("refused")
    {
        CHECK(answered(proto::GrowStatus::Refused) == std::vector<uint32_t> { 0xFFFF'FFFF });
        CHECK(object.wordAt(StartGrowAddress) == 0);
    }
}

TEST_CASE("HostServer.grantedGrowthThatCannotBeOpenedEndsTheHost", "[vtextension][host]")
{
    // The parent counts granted bytes as the memory's: a host that cannot open them fails itself, rather than
    // let the guest see a refusal the parent never gave.
    auto const failingToOpen = [] {
        auto config = HostServerConfig {};
        config.openGuestMemory = [](GuestMemoryMapping& /*memory*/, uint64_t /*bytes*/) {
            return std::expected<void, MappingError> { std::unexpected(MappingError::Io) };
        };
        return config;
    };

    SECTION("on a lane")
    {
        auto growing = GrowingParent { failingToOpen() };
        auto& parent = *growing.parent;
        auto const object = ParentObject {};
        instantiateGranted(parent, object);
        parent.send(0,
                    proto::Call {
                        .instanceId = 1, .exportOrdinal = ordinalOf(growing.loaded, "grow"), .argv = { 1 } });
        REQUIRE(std::holds_alternative<proto::Grow>(parent.receive(0)));
        parent.send(0, proto::Grown { .status = proto::GrowStatus::Granted });
        CHECK(parent.exitWithinDeadline() == ServerExit::MemoryUnusable);
        // The call is never answered.
        CHECK_FALSE(parent.lanes.at(0).receive().has_value());
    }

    SECTION("in a start function")
    {
        auto parent = sharingParent(failingToOpen());
        parent->skipHello();
        parent->createRuntime(1);
        std::ignore = parent->loadOk(1, 1, toModule(vtextension::wasm::testing::StartGrowModule));
        auto const object = ParentObject {};
        auto const grant = object.grant();
        sendGranted(*parent,
                    proto::MemoryGrant {
                        .handle = grant.wireValue, .objectBytes = ObjectBytes, .initialBytes = GuestPage },
                    grant);
        REQUIRE(std::holds_alternative<proto::Grow>(parent->receiveControl()));
        parent->sendControl(proto::Grown { .status = proto::GrowStatus::Granted });
        CHECK(parent->exitWithinDeadline() == ServerExit::MemoryUnusable);
        // The instance is never announced.
        CHECK_FALSE(parent->control->receive().has_value());
    }
}

TEST_CASE("HostServer.wrongGrantIsAViolation", "[vtextension][host]")
{
    /// Whether the object travels with the request.
    enum class Granted : uint8_t
    {
        Nothing, ///< Nothing is granted.
        Object,  ///< The object the parent made is.
    };
    /// One way a grant is not the object it must be.
    struct WrongGrant
    {
        std::string_view what;
        uint64_t madeBytes;        ///< The size of the object the parent makes.
        proto::MemoryGrant memory; ///< What the Instantiate says; a handle of 0 is the grant's own.
        Granted granted;           ///< Whether the object is granted at all.
    };
    // clang-format off
    auto const rows = std::to_array<WrongGrant>({
        { .what = "an object other than the runtime's cap sizes", .madeBytes = ObjectBytes + GuestPage,
          .memory = { .handle = 0, .objectBytes = ObjectBytes + GuestPage, .initialBytes = GuestPage }, .granted = Granted::Object },
        { .what = "more initial bytes than the object", .madeBytes = ObjectBytes,
          .memory = { .handle = 0, .objectBytes = ObjectBytes, .initialBytes = ObjectBytes + GuestPage }, .granted = Granted::Object },
        { .what = "initial bytes that are no whole page", .madeBytes = ObjectBytes,
          .memory = { .handle = 0, .objectBytes = ObjectBytes, .initialBytes = GuestPage + 4096 }, .granted = Granted::Object },
        { .what = "no object granted", .madeBytes = ObjectBytes,
          .memory = { .handle = 0, .objectBytes = ObjectBytes, .initialBytes = GuestPage }, .granted = Granted::Nothing },
        { .what = "an object half the size", .madeBytes = ObjectBytes / 2,
          .memory = { .handle = 0, .objectBytes = ObjectBytes, .initialBytes = GuestPage }, .granted = Granted::Object },
#ifdef _WIN32
        { .what = "a value naming no object", .madeBytes = ObjectBytes,
          .memory = { .handle = 0x1F4, .objectBytes = ObjectBytes, .initialBytes = GuestPage }, .granted = Granted::Nothing },
#else
        { .what = "a handle value beside the descriptor", .madeBytes = ObjectBytes,
          .memory = { .handle = 1, .objectBytes = ObjectBytes, .initialBytes = GuestPage }, .granted = Granted::Object },
#endif
    });
    // clang-format on
    for (auto const& row: rows)
    {
        INFO(row.what);
        auto parent = sharingParent();
        parent->skipHello();
        parent->createRuntime(1);
        std::ignore = parent->loadOk(1, 1, toModule(vtextension::wasm::testing::GrowModule));
        auto const object = ParentObject { row.madeBytes };
        auto grant = row.granted == Granted::Object ? object.grant() : HandleGrant {};
        auto memory = row.memory;
        if (memory.handle == 0)
            memory.handle = grant.wireValue;
        sendGranted(*parent, memory, grant);
        CHECK(parent->exitWithinDeadline() == ServerExit::ProtocolViolation);
    }
}

TEST_CASE("HostServer.anythingButGrownAnswersAGrowIsAViolation", "[vtextension][host]")
{
    SECTION("on a lane")
    {
        for (auto const& answer:
             { proto::Pdu { proto::HostResult {} },
               proto::Pdu { proto::MemoryRead { .instanceId = 1, .address = 0, .size = 1 } } })
        {
            auto growing = GrowingParent {};
            auto& parent = *growing.parent;
            auto const object = ParentObject {};
            instantiateGranted(parent, object);
            parent.send(0,
                        proto::Call { .instanceId = 1,
                                      .exportOrdinal = ordinalOf(growing.loaded, "grow"),
                                      .argv = { 1 } });
            REQUIRE(std::holds_alternative<proto::Grow>(parent.receive(0)));
            parent.send(0, answer);
            CHECK(parent.exitWithinDeadline() == ServerExit::ProtocolViolation);
        }
    }

    SECTION("on control")
    {
        auto parent = sharingParent();
        parent->skipHello();
        parent->createRuntime(1);
        std::ignore = parent->loadOk(1, 1, toModule(vtextension::wasm::testing::StartGrowModule));
        auto const object = ParentObject {};
        auto const grant = object.grant();
        sendGranted(*parent,
                    proto::MemoryGrant {
                        .handle = grant.wireValue, .objectBytes = ObjectBytes, .initialBytes = GuestPage },
                    grant);
        REQUIRE(std::holds_alternative<proto::Grow>(parent->receiveControl()));
        parent->sendControl(proto::Release { .target = proto::ReleaseTarget::Module, .id = 1 });
        CHECK(parent->exitWithinDeadline() == ServerExit::ProtocolViolation);
    }
}

TEST_CASE("HostServer.copyEngineRefusesAGrant", "[vtextension][host]")
{
    auto const grants = [] {
        auto rows = std::vector<std::pair<std::string_view, proto::MemoryGrant>> {
            { "an object's sizes", { .handle = 0, .objectBytes = ObjectBytes, .initialBytes = GuestPage } },
            { "initial bytes alone", { .handle = 0, .objectBytes = 0, .initialBytes = GuestPage } },
            { "a handle value alone", { .handle = 4, .objectBytes = 0, .initialBytes = 0 } },
        };
#ifndef _WIN32
        // An empty grant that a descriptor travels with all the same.
        rows.emplace_back("a descriptor", proto::MemoryGrant {});
#endif
        return rows;
    }();
    for (auto const& [what, memory]: grants)
    {
        INFO(what);
        auto parent = ScriptedParent {};
        parent.skipHello();
        parent.createRuntime(1);
        std::ignore = parent.loadOk(1, 1, toModule(vtextension::wasm::testing::GrowModule));
        auto const object = ParentObject {};
        // The descriptor travels only with the empty grant; Windows hands the copy engine no handle at all.
        auto const grant = memory == proto::MemoryGrant {} ? object.grant() : HandleGrant {};
        sendGranted(parent, memory, grant);
        CHECK(parent.exitWithinDeadline() == ServerExit::ProtocolViolation);
    }
}

#ifndef _WIN32
TEST_CASE("HostServer.onlyAnInstantiateBringsAHandle", "[vtextension][host]")
{
    // A descriptor travelling with anything but an Instantiate is claimed by nothing: a violation, closed
    // with the connection. (On Windows nothing travels beside a frame.)
    auto growing = GrowingParent {};
    auto& parent = *growing.parent;
    auto const object = ParentObject {};
    auto const grant = object.grant();
    REQUIRE(parent.control->send(proto::Release { .target = proto::ReleaseTarget::Module, .id = 1 }, grant)
                .has_value());
    CHECK(parent.exitWithinDeadline() == ServerExit::ProtocolViolation);
}
#endif

TEST_CASE("InProcessHost.connectStartsAServingHost", "[vtextension][host]")
{
    auto connector = InProcessHostConnector { [] { return makeHostedEngine(); }, HostServerConfig {} };
    auto connection = connector.connect();
    REQUIRE(connection.has_value());
    CHECK(connection->lanes.size() == MaxLaneChannels);
    CHECK(connector.connects() == 1);
    CHECK(connector.liveHosts() == 1);

    auto control = parentControl(std::move(connection->control), proto::PduLimits {});
    auto const hello = control.receive();
    REQUIRE(hello.has_value());
    CHECK(std::holds_alternative<proto::Hello>(*hello));

    CHECK(connector.terminateAll() == 1);
    CHECK(control.receive() == std::unexpected(StreamError::Closed));
    connection->handle.reset();
    CHECK(connector.liveHosts() == 0);
    CHECK(connector.terminateAll() == 0);
}

TEST_CASE("InProcessHost.droppingTheHandleStopsTheHost", "[vtextension][host]")
{
    auto connector = InProcessHostConnector { [] { return makeHostedEngine(); }, HostServerConfig {} };
    auto connection = connector.connect();
    REQUIRE(connection.has_value());
    auto control = parentControl(std::move(connection->control), proto::PduLimits {});
    REQUIRE(control.receive().has_value());

    connection->handle.reset();
    CHECK(connector.liveHosts() == 0);
    CHECK(control.receive() == std::unexpected(StreamError::Closed));
}

TEST_CASE("InProcessHost.handleInterrupts", "[vtextension][host]")
{
    auto connector = InProcessHostConnector { [] { return makeHostedEngine(); }, HostServerConfig {} };
    auto connection = connector.connect();
    REQUIRE(connection.has_value());
    auto control = parentControl(std::move(connection->control), proto::PduLimits {});
    auto lane = parentLane(std::move(connection->lanes.front()), proto::PduLimits {});
    REQUIRE(control.receive().has_value());

    REQUIRE(control
                .send(proto::CreateRuntime {
                    .runtimeId = 1,
                    .memoryBytes = contract::Limits.memoryBytes,
                    .stackBytes = contract::Limits.stackBytes,
                    .functions = { proto::WireHostFunction { .module = std::string { contract::HostModule },
                                                             .name = "ping",
                                                             .parameterCount = 1 },
                                   proto::WireHostFunction { .module = std::string { contract::HostModule },
                                                             .name = "pair",
                                                             .parameterCount = 2 } } })
                .has_value());
    REQUIRE(control.receive().has_value());
    REQUIRE(
        control
            .send(proto::Load { .runtimeId = 1, .moduleId = 1, .bytes = toModule(contract::InstanceModule) })
            .has_value());
    auto const loaded = control.receive();
    REQUIRE(loaded.has_value());
    REQUIRE(std::holds_alternative<proto::Loaded>(*loaded));
    REQUIRE(
        control.send(proto::Instantiate { .moduleId = 1, .instanceId = 7, .interruptSlot = 9 }).has_value());
    REQUIRE(control.receive().has_value());

    REQUIRE(lane.send(proto::Call { .instanceId = 7,
                                    .exportOrdinal = ordinalOf(std::get<proto::Loaded>(*loaded), "spin"),
                                    .argv = {} })
                .has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds { 50 });
    // Through the interrupt page and the wake signal, as a host process is interrupted.
    connection->handle->interrupt(9);
    auto received = std::async(std::launch::async, [&lane] { return lane.receive(); });
    if (received.wait_for(InterruptDeadline) != std::future_status::ready)
    {
        // The interrupt was lost: the host is stopped, which ends the wait.
        FAIL_CHECK("the interrupted call did not return");
        std::ignore = connector.terminateAll();
    }
    CHECK(received.get()
          == proto::Pdu { proto::CallResult { .failure = CallFailure::Interrupted, .argv = {} } });
}

TEST_CASE("InProcessHost.noEngineIsHostUnavailable", "[vtextension][host]")
{
    auto connector =
        InProcessHostConnector { [] { return std::unique_ptr<vtextension::wasm::host::IHostedEngine> {}; },
                                 HostServerConfig {} };
    CHECK(connector.connect().error() == ExtensionError::HostUnavailable);
    CHECK(connector.connects() == 0);
}
