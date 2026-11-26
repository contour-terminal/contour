// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/host/HostServer.hpp>

#include <core/Utils.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <latch>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <vtextension/ExtensionError.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>
#include <vtextension/wasm/host/ExecutorThread.hpp>
#include <vtextension/wasm/host/MemoryProvision.hpp>
#include <vtextension/wasm/remote/proto/Pdu.hpp>
#include <vtextension/wasm/remote/transport/ControlPage.hpp>
#include <vtextension/wasm/remote/transport/GuestMemoryMapping.hpp>
#include <vtextension/wasm/remote/transport/HandleGrant.hpp>
#include <vtextension/wasm/remote/transport/PduStream.hpp>

namespace vtextension::wasm::host
{

namespace proto = remote::proto;
using remote::transport::Channel;
using remote::transport::GuestMemoryMapping;
using remote::transport::PduStream;
using remote::transport::StreamError;

namespace
{
    /// What a request was served with: nothing to go on, or the reason the connection ends.
    using Served = std::expected<void, ServerExit>;

    /// The first thread that could not be prepared to run guests, as its engine said, from any thread.
    class Priming
    {
      public:
        /// Notes @p prepared, kept only if it is the first failure.
        void note(std::expected<void, ExtensionError> const& prepared)
        {
            if (prepared)
                return;
            auto const lock = std::scoped_lock { _mutex };
            if (!_failure)
                _failure = prepared.error();
        }

        /// @return Why the first thread that failed was not prepared; nothing if every one was.
        [[nodiscard]] std::optional<ExtensionError> failure() const
        {
            auto const lock = std::scoped_lock { _mutex };
            return _failure;
        }

      private:
        mutable std::mutex _mutex;
        std::optional<ExtensionError> _failure;
    };

    /// @return The reason a stream's failure ends the connection with.
    [[nodiscard]] ServerExit exitOf(StreamError error) noexcept
    {
        switch (error)
        {
            case StreamError::Closed:
            case StreamError::Io: return ServerExit::PeerClosed;
            case StreamError::Malformed:
            case StreamError::OverCap: return ServerExit::ProtocolViolation;
        }
        return ServerExit::ProtocolViolation;
    }

    /// Sends @p pdu on @p stream.
    /// @return Nothing once it is sent; why the connection ends if it could not be.
    [[nodiscard]] Served send(PduStream& stream, proto::Pdu const& pdu)
    {
        return stream.send(pdu).transform_error(exitOf);
    }

    /// Sends @p pdu on @p stream, unless it is larger than its caps allow: @p refusal is sent instead, so
    /// that a reply too large to carry fails its request rather than the connection.
    /// @return Whether @p pdu itself was sent; why the connection ends if neither could be.
    [[nodiscard]] std::expected<proto::Pdu const*, ServerExit> sendOrRefuse(PduStream& stream,
                                                                            proto::Pdu const& pdu,
                                                                            proto::Pdu const& refusal)
    {
        auto const sent = stream.send(pdu);
        if (sent)
            return &pdu;
        if (sent.error() != StreamError::OverCap)
            return std::unexpected(exitOf(sent.error()));
        return send(stream, refusal).transform([&] { return &refusal; });
    }

    /// @return Whether a call can carry @p signature's parameters and results: every value an i32, and no
    /// more
    ///         of them than a Call or a CallResult holds.
    [[nodiscard]] bool isCallable(FunctionSignature const& signature) noexcept
    {
        return signature.isAllI32() && signature.parameters.size() <= proto::MaxCallValues
               && signature.results.size() <= proto::MaxCallValues;
    }

    /// A runtime, and the host function strings it was created with: kept for as long as anything created
    /// from it lives, since a backend's registry of host functions may outlive the call that created it.
    struct RuntimeSlot
    {
        std::vector<proto::WireHostFunction> functions; ///< Declared first, so released last.
        std::shared_ptr<IWasmRuntime> runtime;
        /// Its memory cap: what the object granted to each of its instances is sized for.
        uint64_t memoryBytes = 0;
    };

    /// A module, and the runtime it was loaded on.
    struct ModuleSlot
    {
        std::shared_ptr<RuntimeSlot> runtime; ///< Declared first, so released after the module.
        std::shared_ptr<IWasmModule> module;
    };

    /// One export a Call may name.
    struct CallableExport
    {
        ExportFunction function;    ///< Its handle on the instance.
        std::size_t parameters = 0; ///< How many i32s it takes.
        std::size_t results = 0;    ///< How many i32s it returns.
    };

    /// Whether a MemoryWrite failed since the call it belongs to was served.
    enum class WriteState : uint8_t
    {
        Clean,
        Failed, ///< Fails the instance's next Call, or the HostResult its parked call awaits.
    };

    /// No lane uses an instance.
    constexpr auto NoLane = uint32_t { 0 };

    /// An instance, what may be called on it, and which lane uses it.
    struct InstanceSlot
    {
        std::shared_ptr<ModuleSlot> module; ///< Declared first, so released after the instance.
        std::unique_ptr<IWasmInstance> instance;
        std::unordered_map<uint32_t, CallableExport> exports; ///< By ordinal; fixed once instantiated.
        uint32_t interruptSlot = 0; ///< The slot of the interrupt page it is interrupted by.
        /// The tag of the lane using it (its index + 1), NoLane if none: a call or memory access holds it,
        /// and a lane's nested calls into it share it.
        std::atomic<uint32_t> lane { NoLane };
        /// Whether a MemoryWrite into it failed; touched only by the lane holding it.
        WriteState write = WriteState::Clean;
    };

    /// Whether a lane's hold on an instance is the one that releases it.
    enum class HoldRole : uint8_t
    {
        Nested,    ///< The lane held it already, further up its stack.
        Outermost, ///< The lane took it.
    };

    /// A lane's hold on an instance, for one call or memory access: refused while another lane holds it.
    class InstanceHold
    {
      public:
        /// @return The hold; nothing if another lane holds the instance.
        [[nodiscard]] static std::optional<InstanceHold> take(InstanceSlot& slot, uint32_t laneTag) noexcept
        {
            auto expected = NoLane;
            if (slot.lane.compare_exchange_strong(expected, laneTag))
                return InstanceHold { slot, HoldRole::Outermost };
            if (expected == laneTag)
                return InstanceHold { slot, HoldRole::Nested };
            return std::nullopt;
        }

        InstanceHold(InstanceHold&& other) noexcept:
            _slot { std::exchange(other._slot, nullptr) }, _role { other._role }
        {
        }
        InstanceHold(InstanceHold const&) = delete;
        InstanceHold& operator=(InstanceHold const&) = delete;
        InstanceHold& operator=(InstanceHold&&) = delete;
        ~InstanceHold()
        {
            if (_slot && _role == HoldRole::Outermost)
                _slot->lane.store(NoLane);
        }

      private:
        InstanceHold(InstanceSlot& slot, HoldRole role) noexcept: _slot { &slot }, _role { role } {}

        InstanceSlot* _slot;
        HoldRole _role;
    };
} // namespace

class HostServer::Impl
{
  public:
    Impl(IHostedEngine& engine, std::vector<Channel> lanes, Channel control, HostServerConfig config);

    [[nodiscard]] ServerExit run();
    void stop() noexcept { finish(ServerExit::Stopped); }
    void interruptSlot(uint32_t slot) noexcept;

  private:
    /// One lane, served by an executor thread of its own; only that thread touches it, but for shutdown().
    struct Lane
    {
        Impl* server;                    ///< The server it belongs to.
        PduStream stream;                ///< The helper's end.
        uint32_t tag;                    ///< Its index + 1: what it holds instances with.
        std::optional<ServerExit> ended; ///< Why it stopped serving; nothing while it serves.
        /// The instances whose calls run on the lane, outermost first: one per guest call on its stack.
        std::vector<InstanceSlot*> calls;
        /// The executor thread's preparation: entered at its first call, renewed for a later generation.
        std::unique_ptr<IThreadGuard> guard;
    };

    /// The host context of every instance: a host call is sent on the lane whose call made it.
    class LaneHostContext final: public IHostContext
    {
      public:
        [[nodiscard]] std::expected<void, ExtensionError> callHost(
            std::size_t functionIndex, std::span<uint32_t const> arguments) noexcept override;
    };

    /// The lane whose call runs on this thread; null outside a call, and on the control thread.
    static thread_local Lane* CurrentLane;

    /// The server whose control thread is instantiating on this thread; null outside an instantiation.
    static thread_local Impl* CurrentInstantiation;

    /// Grows instance @p instanceId's memory to @p bytes (GrowRequest): asks the parent on the channel the
    /// guest running on this thread was entered through -- the lane of its call, or control while it is being
    /// instantiated (a start function) -- and opens the bytes once the parent granted them. Anywhere else
    /// growth is refused.
    /// @return Granted once the bytes are open. Refused where nobody can be asked, or when the channel
    ///         failed, was answered with anything but Grown, or the granted bytes could not be opened; each
    ///         of these ends the connection once the guest returned.
    [[nodiscard]] proto::GrowStatus growOnCurrentChannel(uint32_t instanceId,
                                                         GuestMemoryMapping& memory,
                                                         uint64_t bytes) noexcept;

    /// Sends Grow on @p stream and reads its one answer, which must be Grown.
    /// @param ended Why the channel stopped serving; set here if it does, and nothing is asked once set.
    [[nodiscard]] static proto::GrowStatus askToGrow(PduStream& stream,
                                                     std::optional<ServerExit>& ended,
                                                     uint32_t instanceId,
                                                     uint64_t bytes) noexcept;

    /// Checks what an Instantiate grants against how this host shares memory, and maps it.
    /// @return Under MemoryMode::Copy nothing, once the grant is empty and nothing came with it; under
    ///         MemoryMode::Shared the provision of the granted object, mapped, its handle adopted; a
    ///         ProtocolViolation for a grant that is neither.
    [[nodiscard]] std::expected<std::optional<MemoryProvision>, ServerExit> provisionOf(
        proto::Instantiate const& request, RuntimeSlot const& runtime);

    /// Ends the connection for @p reason, unless it ended already: interrupts every live instance, then shuts
    /// every channel down, so that every thread blocked on one returns. Repeating it is harmless.
    void finish(ServerExit reason) noexcept;

    /// Confines the host, says Hello, then serves the control channel (serveControl()).
    void sendHello();
    /// Says Hello as a host that could not prepare a thread to run guests: `Failed`, at `Priming`, with
    /// @p failure.
    void sendUnprimed(ExtensionError failure);

    /// Serves the control channel until the connection ends, and ends it for the reason it does.
    void serveControl();

    /// Releases every instance, module and runtime the parent did not, then the control thread's guard; on
    /// the control thread, once every executor is joined.
    void release() noexcept;

    /// Prepares the calling thread for the engine's current generation, unless it is.
    /// @param guard The thread's guard, replaced if it is for an earlier generation.
    [[nodiscard]] std::expected<void, ExtensionError> prepare(std::unique_ptr<IThreadGuard>& guard);

    // The control channel's requests, on the thread that runs the server.
    [[nodiscard]] Served serve(proto::CreateRuntime const& request);
    [[nodiscard]] Served serve(proto::Load& request);
    [[nodiscard]] Served serve(proto::Instantiate const& request);
    [[nodiscard]] Served serve(proto::Release const& request);
    /// Anything else on control is a violation; the codec refuses what travels the other way already.
    template <typename Other>
    [[nodiscard]] Served serve(Other const& /*request*/)
    {
        return std::unexpected(ServerExit::ProtocolViolation);
    }

    // A lane's requests, on its executor thread.
    void runLane(Lane& lane);
    [[nodiscard]] Served serve(Lane& lane, proto::Call const& request);
    [[nodiscard]] Served serve(Lane& lane, proto::MemoryRead const& request);
    [[nodiscard]] Served serve(Lane& lane, proto::MemoryWrite const& request);
    /// Anything else on a lane is a violation; a HostResult is one too, unless a host call awaits it.
    template <typename Other>
    [[nodiscard]] Served serve(Lane& /*lane*/, Other const& /*request*/)
    {
        return std::unexpected(ServerExit::ProtocolViolation);
    }
    /// Sends a host call on @p lane and serves the lane until its HostResult arrives.
    [[nodiscard]] std::expected<void, ExtensionError> awaitHost(Lane& lane,
                                                                std::size_t functionIndex,
                                                                std::span<uint32_t const> arguments);

    /// @return The live instance @p id; null if there is none.
    [[nodiscard]] std::shared_ptr<InstanceSlot> instanceOf(uint32_t id);

    IHostedEngine& _engine;
    HostServerConfig _config;
    PduStream _control;
    std::vector<Lane> _lanes; ///< Never resized once constructed: each executor holds its lane's address.
    LaneHostContext _hostContext;
    /// The control thread's preparation, for loads and instantiations; renewed for a later generation.
    std::unique_ptr<IThreadGuard> _controlGuard;

    // Every id each kind of object was ever created under; the control thread's alone.
    std::unordered_set<uint32_t> _runtimeIds;
    std::unordered_set<uint32_t> _moduleIds;
    std::unordered_set<uint32_t> _instanceIds;

    std::mutex _mutex; ///< Guards what follows; never held across I/O.
    std::unordered_map<uint32_t, std::shared_ptr<RuntimeSlot>> _runtimes;
    std::unordered_map<uint32_t, std::shared_ptr<ModuleSlot>> _modules;
    std::unordered_map<uint32_t, std::shared_ptr<InstanceSlot>> _instances;
    std::unordered_map<uint32_t, uint32_t> _slotInstances; ///< The live instance at each slot held, by slot.
    std::optional<ServerExit> _exit; ///< Why the connection ended; nothing while it serves.
    /// Why control stopped serving during the instantiation running, through a growth asked there; the
    /// control thread's alone.
    std::optional<ServerExit> _instantiationEnded;
};

thread_local HostServer::Impl::Lane* HostServer::Impl::CurrentLane = nullptr;
thread_local HostServer::Impl* HostServer::Impl::CurrentInstantiation = nullptr;

HostServer::Impl::Impl(IHostedEngine& engine,
                       std::vector<Channel> lanes,
                       Channel control,
                       HostServerConfig config):
    _engine { engine },
    _config { std::move(config) },
    _control { std::move(control), proto::ChannelKind::Control, proto::Direction::ToHelper, _config.limits }
{
    _lanes.reserve(lanes.size());
    for (auto&& [index, channel]: core::views::enumerate(lanes))
        _lanes.push_back(Lane { .server = this,
                                .stream = PduStream { std::move(channel),
                                                      proto::ChannelKind::Lane,
                                                      proto::Direction::ToHelper,
                                                      _config.limits },
                                .tag = static_cast<uint32_t>(index) + 1,
                                .ended = std::nullopt,
                                .calls = {},
                                .guard = nullptr });
}

ServerExit HostServer::Impl::run()
{
    // Destroyed in reverse, on every way out, an exception's included: the connection ends (unblocking every
    // executor), the executors are joined, then every engine object goes, here on the control thread.
    auto releaseAll = core::Finally { [this] { release(); } };
    // Counted down by each executor once it prepared its thread, or failed to; outlives every executor.
    auto lanesPrepared = std::latch { static_cast<std::ptrdiff_t>(_lanes.size()) };
    auto priming = Priming {};
    auto executors = std::vector<ExecutorThread> {};
    auto endConnection = core::Finally { [this] { finish(ServerExit::ProtocolViolation); } };
    executors.reserve(_lanes.size());
    for (auto& lane: _lanes)
    {
        auto started = _config.startExecutor(ExecutorStackBytes, [this, &lane, &lanesPrepared, &priming] {
            auto const prepared = _engine.prepareThread();
            priming.note(prepared);
            lanesPrepared.count_down();
            if (prepared)
                runLane(lane);
        });
        if (!started)
            break;
        executors.push_back(std::move(*started));
    }
    if (executors.size() == _lanes.size())
    {
        // The control thread runs start functions: it is prepared too, then waits for every lane's.
        priming.note(_engine.prepareThread());
        lanesPrepared.wait();
        if (auto const failure = priming.failure())
        {
            // A host that cannot run guests on every thread says so, unconfined, and ends.
            sendUnprimed(*failure);
            finish(ServerExit::ExecutorUnavailable);
        }
        else
            sendHello();
    }
    else
        finish(ServerExit::ExecutorUnavailable);

    // Every channel is shut down by now, and every instance interrupted: each executor returns.
    endConnection.run();
    executors.clear();
    releaseAll.run();
    auto const lock = std::scoped_lock { _mutex };
    return _exit.value_or(ServerExit::Stopped);
}

void HostServer::Impl::sendUnprimed(ExtensionError failure)
{
    // Sent before anything is served: nothing else writes the control channel yet. Whether it arrives or not,
    // the connection ends.
    std::ignore = send(_control,
                       proto::Hello { .version = proto::ProtocolVersion,
                                      .memoryMode = _engine.memoryMode(),
                                      .lockdown = proto::LockdownState::Failed,
                                      .failedStep = proto::LockdownStep::Priming,
                                      .failureCode = std::to_underlying(failure) });
}

void HostServer::Impl::sendHello()
{
    // Every thread the host needs runs now, prepared: it may confine itself, then say how far it did.
    auto const confined =
        _config.confine
            ? _config.confine()
            : std::expected<Confinement, ConfinementFailure> { Confinement { .state = _config.lockdown } };
    auto hello =
        proto::Hello { .version = proto::ProtocolVersion,
                       .memoryMode = _engine.memoryMode(),
                       .lockdown = confined ? confined->state : proto::LockdownState::Failed,
                       .failedStep = proto::LockdownStep::None,
                       .failureCode = 0,
                       .unavailableStep = confined ? confined->unavailableStep : proto::LockdownStep::None };
    if (!confined)
    {
        hello.failedStep = confined.error().step;
        // An HRESULT is negative as an int: its bit pattern is what travels.
        hello.failureCode = static_cast<uint32_t>(confined.error().code);
    }
    if (auto const sent = send(_control, hello); !sent)
    {
        finish(sent.error());
        return;
    }
    serveControl();
}

void HostServer::Impl::serveControl()
{
    while (true)
    {
        auto request = _control.receive();
        if (!request)
        {
            finish(exitOf(request.error()));
            return;
        }
        // Only an Instantiate claims what came with its frame: anything else that brought a handle along is a
        // violation, and the handle is closed with the connection.
        if (!std::holds_alternative<proto::Instantiate>(*request) && _control.takeAttachedHandle())
        {
            finish(ServerExit::ProtocolViolation);
            return;
        }
        auto const served = std::visit([&](auto& pdu) { return serve(pdu); }, *request);
        if (!served)
        {
            finish(served.error());
            return;
        }
    }
}

void HostServer::Impl::release() noexcept
{
    // What the parent never released goes here, still on a prepared thread; outside the lock, which an
    // interrupt() racing this takes.
    auto runtimes = decltype(_runtimes) {};
    auto modules = decltype(_modules) {};
    auto instances = decltype(_instances) {};
    {
        auto const lock = std::scoped_lock { _mutex };
        runtimes = std::exchange(_runtimes, {});
        modules = std::exchange(_modules, {});
        instances = std::exchange(_instances, {});
        _slotInstances.clear();
    }
    instances.clear();
    modules.clear();
    runtimes.clear();
    _controlGuard.reset();
}

void HostServer::Impl::finish(ServerExit reason) noexcept
{
    {
        auto const lock = std::scoped_lock { _mutex };
        if (!_exit)
            _exit = reason;
        for (auto const& [id, slot]: _instances)
            slot->instance->interrupt();
    }
    _control.shutdown();
    for (auto& lane: _lanes)
        lane.stream.shutdown();
}

void HostServer::Impl::interruptSlot(uint32_t slot) noexcept
{
    auto const lock = std::scoped_lock { _mutex };
    auto const held = _slotInstances.find(slot);
    if (held == _slotInstances.end())
        return;
    // The slot may have been read raised for an instance released since, whose slot the parent lowered
    // before it released it, and handed to this one: asked again under the lock that bound it, the page
    // tells this instance's interrupt from that one's.
    if (_config.isInterruptRaised && !_config.isInterruptRaised(slot))
        return;
    if (auto const found = _instances.find(held->second); found != _instances.end())
        found->second->instance->interrupt();
}

std::expected<void, ExtensionError> HostServer::Impl::prepare(std::unique_ptr<IThreadGuard>& guard)
{
    if (guard && _engine.isCurrentThreadPrepared())
        return {};
    // A guard for an earlier generation is replaced; ending it then leaves the new one's preparation alone.
    return _engine.enterThread().transform(
        [&](std::unique_ptr<IThreadGuard> entered) { guard = std::move(entered); });
}

std::shared_ptr<InstanceSlot> HostServer::Impl::instanceOf(uint32_t id)
{
    auto const lock = std::scoped_lock { _mutex };
    auto const found = _instances.find(id);
    return found != _instances.end() ? found->second : nullptr;
}

Served HostServer::Impl::serve(proto::CreateRuntime const& request)
{
    if (!_runtimeIds.insert(request.runtimeId).second)
        return std::unexpected(ServerExit::ProtocolViolation);
    // The engine is handed views of the server's own copy of the strings, which outlives the runtime.
    auto slot = std::make_shared<RuntimeSlot>(RuntimeSlot {
        .functions = request.functions, .runtime = nullptr, .memoryBytes = request.memoryBytes });
    // A stack the executors cannot hold MaxCallDepth of is refused, before the engine sees it.
    // So is a table cap no engine accepts, before it is narrowed to RuntimeLimits' width.
    if (request.stackBytes > proto::MaxRuntimeStackBytes || request.tableElements > MaxTableElements)
        return send(_control,
                    proto::RuntimeCreated { .runtimeId = request.runtimeId,
                                            .error = ExtensionError::RuntimeUnavailable });
    auto table = std::vector<HostFunction> {};
    table.reserve(slot->functions.size());
    for (auto const& function: slot->functions)
        table.push_back(HostFunction {
            .module = function.module, .name = function.name, .parameterCount = function.parameterCount });
    auto created =
        _engine.createRuntime(RuntimeLimits { .memoryBytes = static_cast<std::size_t>(request.memoryBytes),
                                              .stackBytes = static_cast<std::size_t>(request.stackBytes),
                                              .tableElements = static_cast<uint32_t>(request.tableElements) },
                              table);
    auto error = std::optional<ExtensionError> {};
    if (created)
    {
        slot->runtime = std::move(*created);
        auto const lock = std::scoped_lock { _mutex };
        _runtimes.emplace(request.runtimeId, std::move(slot));
    }
    else
        error = created.error();
    return send(_control, proto::RuntimeCreated { .runtimeId = request.runtimeId, .error = error });
}

Served HostServer::Impl::serve(proto::Load& request)
{
    auto runtime = std::shared_ptr<RuntimeSlot> {};
    {
        auto const lock = std::scoped_lock { _mutex };
        if (auto const found = _runtimes.find(request.runtimeId); found != _runtimes.end())
            runtime = found->second;
    }
    if (!runtime || !_moduleIds.insert(request.moduleId).second)
        return std::unexpected(ServerExit::ProtocolViolation);

    // The helper is the host the identity names: there is no other to pick.
    auto loaded = prepare(_controlGuard).and_then([&] {
        return runtime->runtime->load(ModuleIdentity {}, std::move(request.bytes));
    });
    if (!loaded)
        return send(
            _control,
            proto::Loaded {
                .moduleId = request.moduleId, .error = loaded.error(), .imports = {}, .exports = {} });

    auto const& module = *loaded;
    auto const description =
        proto::Pdu { proto::Loaded { .moduleId = request.moduleId,
                                     .error = std::nullopt,
                                     .imports = { module->imports().begin(), module->imports().end() },
                                     .exports = { module->exports().begin(), module->exports().end() } } };
    // A description too large to carry fails the load: the module is dropped, and its id stays used.
    auto const refusal = proto::Pdu { proto::Loaded { .moduleId = request.moduleId,
                                                      .error = ExtensionError::InvalidComponent,
                                                      .imports = {},
                                                      .exports = {} } };
    return sendOrRefuse(_control, description, refusal).transform([&](proto::Pdu const* sent) {
        if (sent != &description)
            return;
        auto const lock = std::scoped_lock { _mutex };
        _modules.emplace(request.moduleId,
                         std::make_shared<ModuleSlot>(ModuleSlot { .runtime = runtime, .module = module }));
    });
}

Served HostServer::Impl::serve(proto::Instantiate const& request)
{
    auto module = std::shared_ptr<ModuleSlot> {};
    {
        auto const lock = std::scoped_lock { _mutex };
        if (auto const found = _modules.find(request.moduleId); found != _modules.end())
            module = found->second;
    }
    // A slot past the page, or one a live instance holds, could interrupt no instance, or the wrong one.
    auto const isSlotFree = [&] {
        auto const lock = std::scoped_lock { _mutex };
        return request.interruptSlot < remote::transport::InterruptSlotCount
               && !_slotInstances.contains(request.interruptSlot);
    };
    if (!module || !isSlotFree() || !_instanceIds.insert(request.instanceId).second)
        return std::unexpected(ServerExit::ProtocolViolation);
    auto provision = provisionOf(request, *module->runtime);
    if (!provision)
        return std::unexpected(provision.error());

    // A start function runs here, on the control thread, where a host call finds no lane and fails, and
    // growing its memory asks the parent on control.
    auto instance = prepare(_controlGuard).and_then([&] {
        auto scope = std::optional<ProvisionScope> {};
        if (*provision)
            scope.emplace(std::move(**provision));
        auto const instantiating = core::Finally { [outer = std::exchange(CurrentInstantiation, this)] {
            CurrentInstantiation = outer;
        } };
        return module->module->instantiate(_hostContext);
    });
    if (auto const ended = std::exchange(_instantiationEnded, std::nullopt))
        return std::unexpected(*ended);
    if (!instance)
        return send(_control,
                    proto::Instantiated {
                        .instanceId = request.instanceId, .error = instance.error(), .callableExports = {} });

    auto slot = std::make_shared<InstanceSlot>();
    slot->module = module;
    slot->instance = std::move(*instance);
    slot->interruptSlot = request.interruptSlot;
    auto callable = std::vector<uint32_t> {};
    for (auto const [ordinal, description]: core::views::enumerate(module->module->exports()))
    {
        if (description.kind != ExternKind::Function || !isCallable(description.signature))
            continue;
        if (auto const function = slot->instance->function(description.name))
        {
            slot->exports.emplace(static_cast<uint32_t>(ordinal),
                                  CallableExport { .function = *function,
                                                   .parameters = description.signature.parameters.size(),
                                                   .results = description.signature.results.size() });
            callable.push_back(static_cast<uint32_t>(ordinal));
        }
    }

    // Live before it is announced: the parent may call it on a lane the moment the answer arrives.
    {
        auto const lock = std::scoped_lock { _mutex };
        if (_exit)
            slot->instance->interrupt();
        _instances.emplace(request.instanceId, slot);
        _slotInstances.emplace(request.interruptSlot, request.instanceId);
    }
    auto const answer = proto::Pdu { proto::Instantiated {
        .instanceId = request.instanceId, .error = std::nullopt, .callableExports = std::move(callable) } };
    auto const refusal = proto::Pdu { proto::Instantiated { .instanceId = request.instanceId,
                                                            .error = ExtensionError::InvalidComponent,
                                                            .callableExports = {} } };
    return sendOrRefuse(_control, answer, refusal).transform([&](proto::Pdu const* sent) {
        if (sent == &answer)
            return;
        // Stopped for good, as a released one is, should a lane have reached it meanwhile.
        auto const lock = std::scoped_lock { _mutex };
        slot->instance->interrupt();
        _instances.erase(request.instanceId);
        _slotInstances.erase(request.interruptSlot);
    });
}

std::expected<std::optional<MemoryProvision>, ServerExit> HostServer::Impl::provisionOf(
    proto::Instantiate const& request, RuntimeSlot const& runtime)
{
    auto const& grant = request.memory;
    if (_engine.memoryMode() == proto::MemoryMode::Copy)
    {
        // A host that copies memory is granted nothing, and nothing travels with the request.
        if (grant != proto::MemoryGrant {} || _control.takeAttachedHandle())
            return std::unexpected(ServerExit::ProtocolViolation);
        return std::nullopt;
    }

    // The object is the one the runtime's cap sizes, and the memory spans whole pages of it at first.
    if (grant.objectBytes != proto::memoryObjectBytes(runtime.memoryBytes)
        || grant.initialBytes > grant.objectBytes || grant.initialBytes % proto::WasmPageBytes != 0)
        return std::unexpected(ServerExit::ProtocolViolation);
    auto claimed = remote::transport::claimGrantedHandle(_control, grant.handle);
    if (!claimed)
        return std::unexpected(ServerExit::ProtocolViolation);
    // What cannot be mapped as the object, writable, is not the object granted: let go of unproven, never
    // closed on Windows, where the value may name a handle of this process's own.
    auto mapping = GuestMemoryMapping::map(
        claimed->get(), grant.objectBytes, grant.initialBytes, _engine.memoryGuardBytes());
    if (!mapping)
        return std::unexpected(ServerExit::ProtocolViolation);
    // Proven, the handle is ours, and closed: the mapping keeps the object alive.
    std::ignore = std::move(*claimed).adopt();
    return MemoryProvision { .memory = std::move(*mapping),
                             .grow = [this, instanceId = request.instanceId](GuestMemoryMapping& memory,
                                                                             uint64_t bytes) {
                                 return growOnCurrentChannel(instanceId, memory, bytes);
                             } };
}

proto::GrowStatus HostServer::Impl::growOnCurrentChannel(uint32_t instanceId,
                                                         GuestMemoryMapping& memory,
                                                         uint64_t bytes) noexcept
{
    auto* stream = static_cast<PduStream*>(nullptr);
    auto* ended = static_cast<std::optional<ServerExit>*>(nullptr);
    if (auto* const lane = CurrentLane; lane != nullptr && lane->server == this)
    {
        stream = &lane->stream;
        ended = &lane->ended;
    }
    else if (CurrentInstantiation == this)
    {
        stream = &_control;
        ended = &_instantiationEnded;
    }
    else
        return proto::GrowStatus::Refused;

    if (askToGrow(*stream, *ended, instanceId, bytes) != proto::GrowStatus::Granted)
        return proto::GrowStatus::Refused;
    // The parent counts the bytes as the memory's now: failing to open them is this host's own failure.
    if (!_config.openGuestMemory(memory, bytes))
    {
        *ended = ServerExit::MemoryUnusable;
        return proto::GrowStatus::Refused;
    }
    return proto::GrowStatus::Granted;
}

proto::GrowStatus HostServer::Impl::askToGrow(PduStream& stream,
                                              std::optional<ServerExit>& ended,
                                              uint32_t instanceId,
                                              uint64_t bytes) noexcept
{
    if (ended)
        return proto::GrowStatus::Refused;
    if (auto const sent = send(stream, proto::Grow { .instanceId = instanceId, .bytes = bytes }); !sent)
    {
        ended = sent.error();
        return proto::GrowStatus::Refused;
    }
    // Exactly one answer: the parent serves nothing else on this channel before it.
    auto const answer = stream.receive();
    if (!answer)
    {
        ended = exitOf(answer.error());
        return proto::GrowStatus::Refused;
    }
    if (auto const* const grown = std::get_if<proto::Grown>(&*answer))
        return grown->status;
    ended = ServerExit::ProtocolViolation;
    return proto::GrowStatus::Refused;
}

Served HostServer::Impl::serve(proto::Release const& request)
{
    // Released outside the lock; a lane still holding it releases it when its call returns.
    auto released = std::shared_ptr<void> {};
    {
        auto const lock = std::scoped_lock { _mutex };
        auto const take = [&](auto& map) {
            if (auto const found = map.find(request.id); found != map.end())
            {
                released = std::move(found->second);
                map.erase(found);
            }
        };
        switch (request.target)
        {
            case proto::ReleaseTarget::Instance:
                // Stopped for good, should a lane still run a call of it: the parent holds it no more, and
                // hands its slot to the next instance.
                if (auto const found = _instances.find(request.id); found != _instances.end())
                {
                    found->second->instance->interrupt();
                    _slotInstances.erase(found->second->interruptSlot);
                }
                take(_instances);
                break;
            case proto::ReleaseTarget::Module: take(_modules); break;
            case proto::ReleaseTarget::Runtime: take(_runtimes); break;
        }
    }
    if (!released)
        return std::unexpected(ServerExit::ProtocolViolation);
    return {};
}

void HostServer::Impl::runLane(Lane& lane)
{
    while (!lane.ended)
    {
        auto request = lane.stream.receive();
        if (!request)
        {
            lane.ended = exitOf(request.error());
            break;
        }
        auto const served = std::visit([&](auto const& pdu) { return serve(lane, pdu); }, *request);
        if (!served)
            lane.ended = served.error();
    }
    finish(*lane.ended);
    // Ended on the thread that entered it.
    lane.guard.reset();
}

Served HostServer::Impl::serve(Lane& lane, proto::Call const& request)
{
    auto const slot = instanceOf(request.instanceId);
    if (!slot)
        return std::unexpected(ServerExit::ProtocolViolation);
    auto const callable = slot->exports.find(request.exportOrdinal);
    if (callable == slot->exports.end() || request.argv.size() != callable->second.parameters)
        return std::unexpected(ServerExit::ProtocolViolation);

    // Each guest call on the lane runs on its executor's stack, with a stack limit of its own: the depth
    // bounds what they take together (ExecutorStackBytes).
    if (lane.calls.size() >= proto::MaxCallDepth)
        return std::unexpected(ServerExit::ProtocolViolation);

    auto result = proto::CallResult {};
    {
        auto const hold = InstanceHold::take(*slot, lane.tag);
        if (!hold)
            return std::unexpected(ServerExit::ProtocolViolation);
        if (std::exchange(slot->write, WriteState::Clean) == WriteState::Failed || !prepare(lane.guard))
            result.failure = CallFailure::Trap;
        else
        {
            auto argv = request.argv;
            argv.resize(std::max(callable->second.parameters, callable->second.results));
            auto const outer = std::exchange(CurrentLane, &lane);
            lane.calls.push_back(slot.get());
            auto const called = slot->instance->call(callable->second.function, argv);
            lane.calls.pop_back();
            CurrentLane = outer;
            if (called)
            {
                argv.resize(callable->second.results);
                result.argv = std::move(argv);
            }
            else
                result.failure = called.error();
        }
    }
    // A lane that ended inside the call (its host call's stream failed, or the parent broke the protocol
    // there) answers nothing more.
    if (lane.ended)
        return std::unexpected(*lane.ended);
    // Released before the answer goes out: the parent may call the instance on another lane once it arrives.
    return send(lane.stream, result);
}

Served HostServer::Impl::serve(Lane& lane, proto::MemoryRead const& request)
{
    auto const slot = instanceOf(request.instanceId);
    if (!slot)
        return std::unexpected(ServerExit::ProtocolViolation);
    auto data = proto::MemoryData {};
    {
        auto const hold = InstanceHold::take(*slot, lane.tag);
        if (!hold)
            return std::unexpected(ServerExit::ProtocolViolation);
        if (auto const view = slot->instance->memory(request.address, request.size))
            data = proto::MemoryData { .status = proto::MemoryStatus::Ok,
                                       .bytes = { view->begin(), view->end() } };
    }
    // A range larger than the stream's memory cap is out of the bounds the parent may read.
    return sendOrRefuse(lane.stream, data, proto::MemoryData {}).transform([](proto::Pdu const*) {});
}

Served HostServer::Impl::serve(Lane& lane, proto::MemoryWrite const& request)
{
    auto const slot = instanceOf(request.instanceId);
    if (!slot)
        return std::unexpected(ServerExit::ProtocolViolation);
    auto const hold = InstanceHold::take(*slot, lane.tag);
    if (!hold)
        return std::unexpected(ServerExit::ProtocolViolation);
    // The stream's memory cap keeps the size within 32 bits.
    if (auto const view =
            slot->instance->memory(request.address, static_cast<uint32_t>(request.bytes.size())))
        std::ranges::copy(request.bytes, view->begin());
    else
        slot->write = WriteState::Failed;
    return {};
}

std::expected<void, ExtensionError> HostServer::Impl::awaitHost(Lane& lane,
                                                                std::size_t functionIndex,
                                                                std::span<uint32_t const> arguments)
{
    // Every way out but a HostResult fails the host call, which stops the guest's call; the lane's reason
    // then ends the connection once that call returned.
    auto const sent = send(lane.stream,
                           proto::CallHost { .functionIndex = static_cast<uint32_t>(functionIndex),
                                             .arguments = { arguments.begin(), arguments.end() } });
    if (!sent)
    {
        lane.ended = sent.error();
        return std::unexpected(ExtensionError::HostUnavailable);
    }
    while (!lane.ended)
    {
        auto request = lane.stream.receive();
        if (!request)
        {
            lane.ended = exitOf(request.error());
            break;
        }
        if (auto const* result = std::get_if<proto::HostResult>(&*request))
        {
            // A write into the parked call's instance that failed fails that call.
            if (std::exchange(lane.calls.back()->write, WriteState::Clean) == WriteState::Failed)
                return std::unexpected(ExtensionError::Trapped);
            if (result->error)
                return std::unexpected(*result->error);
            return {};
        }
        // A call back in, or the guest's memory: served on this thread, as the parent is parked on this lane.
        auto const served = std::visit([&](auto const& pdu) { return serve(lane, pdu); }, *request);
        if (!served)
            lane.ended = served.error();
    }
    return std::unexpected(ExtensionError::HostUnavailable);
}

std::expected<void, ExtensionError> HostServer::Impl::LaneHostContext::callHost(
    std::size_t functionIndex, std::span<uint32_t const> arguments) noexcept
{
    // A host call no lane's call made (a start function's, during instantiation) has nowhere to go.
    auto* const lane = CurrentLane;
    if (!lane)
        return std::unexpected(ExtensionError::RuntimeUnavailable);
    return lane->server->awaitHost(*lane, functionIndex, arguments);
}

HostServer::HostServer(IHostedEngine& engine,
                       std::vector<Channel> lanes,
                       Channel control,
                       HostServerConfig config):
    _impl { std::make_unique<Impl>(engine, std::move(lanes), std::move(control), std::move(config)) }
{
}

HostServer::~HostServer() = default;

ServerExit HostServer::run()
{
    return _impl->run();
}

void HostServer::stop() noexcept
{
    _impl->stop();
}

void HostServer::interruptSlot(uint32_t slot) noexcept
{
    _impl->interruptSlot(slot);
}

} // namespace vtextension::wasm::host
