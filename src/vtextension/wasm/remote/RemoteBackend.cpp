// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/remote/RemoteBackend.hpp>

#include <core/log/LogStore.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <vtextension/ExtensionError.hpp>
#include <vtextension/ExtensionWorkerPool.hpp>
#include <vtextension/wasm/WasmBinary.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>
#include <vtextension/wasm/remote/CopyPathMemory.hpp>
#include <vtextension/wasm/remote/HelperIncarnation.hpp>
#include <vtextension/wasm/remote/HostConnector.hpp>
#include <vtextension/wasm/remote/HostRefusals.hpp>
#include <vtextension/wasm/remote/LaneWatch.hpp>
#include <vtextension/wasm/remote/RemoteKindState.hpp>
#include <vtextension/wasm/remote/proto/Pdu.hpp>
#include <vtextension/wasm/remote/transport/PduStream.hpp>

namespace vtextension::wasm::remote
{

using transport::PduStream;
using transport::StreamError;

// Every worker calls on a lane of its own: a host serving fewer would leave a worker without one.
static_assert(MaxLaneChannels == MaxExtensionWorkers, "one lane per extension worker");

namespace
{
    /// The largest instance memory the protocol carries (one MemoryData or MemoryWrite holds at most this
    /// much), and so the limit of the memory the parent's copy reaches, whatever its runtime's cap: no view
    /// reaches past it.
    constexpr auto MaxCopiedBytes = static_cast<std::size_t>(proto::PduLimits {}.maxMemoryBytes);

    /// What a host may refuse a module or an instance with; any other refusal breaks the protocol. What the
    /// others say (a trap, a deadline, a host gone) cannot be the outcome of loading or instantiating in a
    /// host that serves, and believing it would let a host blame the extension for its own failure.
    constexpr auto HostRefusals = std::array { ExtensionError::InvalidComponent,
                                               ExtensionError::OutOfMemory,
                                               ExtensionError::RuntimeUnavailable,
                                               ExtensionError::ModuleTooLarge };

    /// @return Whether a host may refuse a module or an instance with @p error.
    [[nodiscard]] constexpr bool isHostRefusal(ExtensionError error) noexcept
    {
        return std::ranges::contains(HostRefusals, error);
    }

    /// Whether an instance was interrupted (IWasmInstance::interrupt()).
    enum class InterruptState : uint8_t
    {
        Running,     ///< Not interrupted: its calls run.
        Interrupted, ///< Interrupted, for good: no call of it runs again.
    };

    /// A frame of the parent's on a lane, as the backstop sees it (LaneWatch): from a call's or a memory
    /// read's first byte sent until its last byte received.
    class LaneFrame
    {
      public:
        LaneFrame(LaneWatch& watch, uint32_t instanceId) noexcept: _watch { watch }
        {
            _watch.enter(instanceId);
        }
        LaneFrame(LaneFrame const&) = delete;
        LaneFrame(LaneFrame&&) = delete;
        LaneFrame& operator=(LaneFrame const&) = delete;
        LaneFrame& operator=(LaneFrame&&) = delete;
        ~LaneFrame() { _watch.leave(); }

      private:
        LaneWatch& _watch;
    };

    /// The innermost frame of a lane running a host call in the parent: time the backstop's grace does not
    /// count, since the parent is the slow one then.
    class HostCallFrame
    {
      public:
        explicit HostCallFrame(LaneWatch& watch) noexcept: _watch { watch } { _watch.beginHostCall(); }
        HostCallFrame(HostCallFrame const&) = delete;
        HostCallFrame(HostCallFrame&&) = delete;
        HostCallFrame& operator=(HostCallFrame const&) = delete;
        HostCallFrame& operator=(HostCallFrame&&) = delete;
        ~HostCallFrame() { _watch.endHostCall(); }

      private:
        LaneWatch& _watch;
    };

    /// A module as one incarnation describes it, under the id it was loaded with there.
    struct LoadedModule
    {
        uint32_t moduleId = 0;                  ///< Its id in the incarnation.
        std::vector<ImportDescription> imports; ///< What it imports, as described.
        std::vector<ExportDescription> exports; ///< What it exports, as described.
    };

    /// Loads @p bytes in @p incarnation, and checks the description against what the module declares: a host
    /// describing more imports or exports than the module has is not believed.
    /// @return The module's description; the host's own error if it refused the module (one of HostRefusals),
    ///         `InvalidComponent` if the module is too large to send, `HostUnavailable` if the host died or
    ///         broke the protocol, a refusal of any other kind included.
    [[nodiscard]] std::expected<LoadedModule, ExtensionError> loadInto(HelperIncarnation& incarnation,
                                                                       std::span<std::byte const> bytes)
    {
        auto const moduleId = incarnation.nextModuleId();
        auto reply = incarnation.request(proto::Load { .runtimeId = HelperIncarnation::RuntimeId,
                                                       .moduleId = moduleId,
                                                       .bytes = { bytes.begin(), bytes.end() } },
                                         HostStep::Load);
        // A module too large to send is the extension's fault; anything else, an over-cap reply included, the
        // host's, which then is dead.
        if (!reply)
            return std::unexpected(reply.error() == StreamError::OverCap && incarnation.isAlive()
                                       ? ExtensionError::InvalidComponent
                                       : ExtensionError::HostUnavailable);
        auto* const loaded = std::get_if<proto::Loaded>(&*reply);
        if (!loaded || loaded->moduleId != moduleId || (loaded->error && !isHostRefusal(*loaded->error)))
        {
            incarnation.markDead();
            return std::unexpected(ExtensionError::HostUnavailable);
        }
        if (loaded->error)
            return std::unexpected(*loaded->error);
        auto const declared = binary::declaredCounts(bytes);
        if (!declared || loaded->imports.size() > declared->imports
            || loaded->exports.size() > declared->exports)
        {
            incarnation.markDead();
            return std::unexpected(ExtensionError::HostUnavailable);
        }
        return LoadedModule { .moduleId = moduleId,
                              .imports = std::move(loaded->imports),
                              .exports = std::move(loaded->exports) };
    }

    /// Whether a backend still starts hosts.
    enum class RosterState : uint8_t
    {
        Closed, ///< Its backend went: its hosts were let go, and no more start.
        Open,   ///< Hosts start, and are listed.
    };

    /// The incarnations one backend started, so that they go with it: a backend let go while a thread still
    /// uses one of its runtimes -- one blocked on a host that stopped reading, say -- lets its hosts go
    /// rather than wait for that thread. Thread-safe; nothing it does blocks.
    class IncarnationRoster
    {
      public:
        /// @return Whether hosts still start.
        [[nodiscard]] bool isOpen() const
        {
            auto const lock = std::scoped_lock { _mutex };
            return _state == RosterState::Open;
        }

        /// Lists @p incarnation, or lets it go at once if the backend went meanwhile.
        /// @return Whether it was listed.
        [[nodiscard]] bool enlist(std::shared_ptr<HelperIncarnation> const& incarnation)
        {
            {
                auto const lock = std::scoped_lock { _mutex };
                if (_state == RosterState::Open)
                {
                    std::erase_if(_listed, [](auto const& listed) { return listed.expired(); });
                    _listed.push_back(incarnation);
                    return true;
                }
            }
            incarnation->letGo();
            return false;
        }

        /// Lets every listed incarnation go, and starts none from now on.
        void close() noexcept
        {
            auto listed = std::vector<std::weak_ptr<HelperIncarnation>> {};
            {
                auto const lock = std::scoped_lock { _mutex };
                _state = RosterState::Closed;
                // The whole list, expired entries with the rest: none stays behind, and nothing is listed
                // again once closed (enlist()).
                listed = std::exchange(_listed, {});
            }
            for (auto const& entry: listed)
                if (auto const incarnation = entry.lock())
                    incarnation->letGo();
        }

      private:
        mutable std::mutex _mutex; ///< Guards what follows.
        RosterState _state = RosterState::Open;
        std::vector<std::weak_ptr<HelperIncarnation>> _listed;
    };

    /// The host a runtime runs the modules of one identity in: its current incarnation, started anew when the
    /// last one died.
    class HelperSlot
    {
      public:
        /// @param connector What starts each incarnation.
        /// @param runtime The runtime each incarnation is created with.
        /// @param policy What each incarnation's host is held to.
        /// @param roster Where each incarnation is listed, to go with the backend.
        /// @param refusals Where why a host was refused is logged: the backend's.
        HelperSlot(std::shared_ptr<IncarnationRoster> roster,
                   std::shared_ptr<IHostConnector> connector,
                   std::shared_ptr<HostRefusalLog> refusals,
                   proto::CreateRuntime runtime,
                   HelperPolicy policy) noexcept:
            _roster { std::move(roster) },
            _connector { std::move(connector) },
            _refusals { std::move(refusals) },
            _runtime { std::move(runtime) },
            _policy { policy }
        {
        }

        /// @return The incarnation serving; a new one if none is; `HostUnavailable` once the backend went.
        [[nodiscard]] std::expected<std::shared_ptr<HelperIncarnation>, ExtensionError> live()
        {
            auto const lock = std::scoped_lock { _mutex };
            if (_current && _current->isAlive())
                return _current;
            if (!_connector || !_roster->isOpen())
                return std::unexpected(ExtensionError::HostUnavailable);
            return HelperIncarnation::connect(*_connector, _runtime, _policy, *_refusals)
                .and_then([this](std::shared_ptr<HelperIncarnation> started)
                              -> std::expected<std::shared_ptr<HelperIncarnation>, ExtensionError> {
                    if (!_roster->enlist(started))
                        return std::unexpected(ExtensionError::HostUnavailable);
                    if (_current)
                        errorLog()("An extension host stopped; it was started anew ({} times so far).",
                                   ++_respawns);
                    _current = std::move(started);
                    return _current;
                });
        }

      private:
        std::shared_ptr<IncarnationRoster> _roster;
        std::shared_ptr<IHostConnector> _connector;
        std::shared_ptr<HostRefusalLog> _refusals;
        proto::CreateRuntime _runtime;
        HelperPolicy _policy;
        std::mutex _mutex; ///< Guards what follows.
        std::shared_ptr<HelperIncarnation> _current;
        std::size_t _respawns = 0; ///< How often a dead incarnation was replaced.
    };

    /// A runtime whose modules run in hosts: one per identity, and one per module of no identity.
    class RemoteRuntime final: public IWasmRuntime, public std::enable_shared_from_this<RemoteRuntime>
    {
      public:
        /// Counts the runtime alive in @p kind for as long as it lives.
        RemoteRuntime(std::shared_ptr<RemoteKindState> kind,
                      std::shared_ptr<IncarnationRoster> roster,
                      std::shared_ptr<IHostConnector> connector,
                      std::shared_ptr<HostRefusalLog> refusals,
                      HelperPolicy policy,
                      RuntimeLimits limits,
                      proto::CreateRuntime create) noexcept:
            _kind { std::move(kind) },
            _roster { std::move(roster) },
            _connector { std::move(connector) },
            _refusals { std::move(refusals) },
            _policy { policy },
            _limits { limits },
            _create { std::move(create) }
        {
            _kind->acquireRuntime();
        }

        RemoteRuntime(RemoteRuntime const&) = delete;
        RemoteRuntime(RemoteRuntime&&) = delete;
        RemoteRuntime& operator=(RemoteRuntime const&) = delete;
        RemoteRuntime& operator=(RemoteRuntime&&) = delete;
        ~RemoteRuntime() override { _kind->releaseRuntime(); }

        [[nodiscard]] std::expected<std::shared_ptr<IWasmModule>, ExtensionError> load(
            ModuleIdentity identity, std::vector<std::byte> bytes) override;

        /// @return The kind it is counted alive in.
        [[nodiscard]] RemoteKindState& kind() const noexcept { return *_kind; }

        /// @return What every instance is held to.
        [[nodiscard]] RuntimeLimits limits() const noexcept { return _limits; }

        /// @return Its host functions, as CallHost indexes them.
        [[nodiscard]] std::span<proto::WireHostFunction const> functions() const noexcept
        {
            return _create.functions;
        }

      private:
        /// @return The slot of the host modules of @p key run in; a slot of its own for an empty key.
        [[nodiscard]] std::shared_ptr<HelperSlot> slotFor(std::string const& key)
        {
            if (key.empty())
                return std::make_shared<HelperSlot>(_roster, _connector, _refusals, _create, _policy);
            auto const lock = std::scoped_lock { _mutex };
            // The hosts of identities whose last module went are forgotten.
            std::erase_if(_slots, [](auto const& entry) { return entry.second.expired(); });
            auto& entry = _slots[key];
            auto slot = entry.lock();
            if (!slot)
            {
                slot = std::make_shared<HelperSlot>(_roster, _connector, _refusals, _create, _policy);
                entry = slot;
            }
            return slot;
        }

        std::shared_ptr<RemoteKindState> _kind;
        std::shared_ptr<IncarnationRoster> _roster;
        std::shared_ptr<IHostConnector> _connector;
        std::shared_ptr<HostRefusalLog> _refusals;
        HelperPolicy _policy;
        RuntimeLimits _limits;
        proto::CreateRuntime _create; ///< What creates the runtime in each of its hosts.
        std::mutex _mutex;            ///< Guards _slots.
        /// The host of each identity, while a module of it lives.
        std::map<std::string, std::weak_ptr<HelperSlot>, std::less<>> _slots;
    };

    class RemoteInstance;

    /// A module loaded in a host, which it keeps the bytes of: a host that died gets them again, from the
    /// next instantiate().
    class RemoteModule final: public IWasmModule, public std::enable_shared_from_this<RemoteModule>
    {
      public:
        RemoteModule(std::shared_ptr<RemoteRuntime> runtime,
                     std::shared_ptr<HelperSlot> slot,
                     std::vector<std::byte> bytes,
                     std::shared_ptr<HelperIncarnation> loadedIn,
                     LoadedModule loaded) noexcept:
            _runtime { std::move(runtime) },
            _slot { std::move(slot) },
            _bytes { std::move(bytes) },
            _imports { std::move(loaded.imports) },
            _exports { std::move(loaded.exports) },
            _loadedIn { std::move(loadedIn) },
            _moduleId { loaded.moduleId }
        {
        }

        RemoteModule(RemoteModule const&) = delete;
        RemoteModule(RemoteModule&&) = delete;
        RemoteModule& operator=(RemoteModule const&) = delete;
        RemoteModule& operator=(RemoteModule&&) = delete;

        ~RemoteModule() override
        {
            _loadedIn->release(proto::Release { .target = proto::ReleaseTarget::Module, .id = _moduleId });
        }

        [[nodiscard]] std::span<ImportDescription const> imports() const noexcept override
        {
            return _imports;
        }
        [[nodiscard]] std::span<ExportDescription const> exports() const noexcept override
        {
            return _exports;
        }

        /// Instantiates the module in its host, loading it again first if the host it was loaded in died.
        ///
        /// A respawned host that describes the module's bytes otherwise than its predecessor did fails
        /// `InvalidComponent`, not `HostUnavailable`, although the host is the one at fault and is
        /// terminated: `HostUnavailable` says the host could not be reached, which a caller may meet by
        /// trying again in a new one, whereas this module's imports and exports were checked against its
        /// world as first described, and what a host would run now is not that module. Refusing the module is
        /// what keeps it from running unchecked; the next instantiation respawns a host and checks again.
        /// @return The instance; `InvalidComponent` if a respawned host describes the module differently,
        ///         `HostUnavailable` if the host died or broke the protocol, else the host's own error.
        [[nodiscard]] std::expected<std::unique_ptr<IWasmInstance>, ExtensionError> instantiate(
            IHostContext& context) override;

        /// @return The runtime it was loaded on.
        [[nodiscard]] RemoteRuntime const& runtime() const noexcept { return *_runtime; }

      private:
        std::shared_ptr<RemoteRuntime> _runtime; // First, so that it goes last.
        std::shared_ptr<HelperSlot> _slot;
        std::vector<std::byte> _bytes;
        std::vector<ImportDescription> _imports;
        std::vector<ExportDescription> _exports;
        std::mutex _mutex; ///< Guards what follows.
        std::shared_ptr<HelperIncarnation> _loadedIn;
        uint32_t _moduleId;
    };

    /// An instance in a host. Its calls, host calls and memory accesses travel on the calling thread's lane.
    class RemoteInstance final: public IWasmInstance
    {
      public:
        RemoteInstance(std::shared_ptr<RemoteModule> module,
                       std::shared_ptr<HelperIncarnation> incarnation,
                       uint32_t instanceId,
                       uint32_t interruptSlot,
                       std::vector<uint32_t> callableExports,
                       IHostContext& context):
            _module { std::move(module) },
            _incarnation { std::move(incarnation) },
            _instanceId { instanceId },
            _interruptSlot { interruptSlot },
            _callableExports { std::move(callableExports) },
            _context { context },
            _memory { instanceId,
                      std::min(_module->runtime().limits().memoryBytes, MaxCopiedBytes),
                      [this](uint32_t address, uint32_t size) { return fetch(address, size); } }
        {
        }

        RemoteInstance(RemoteInstance const&) = delete;
        RemoteInstance(RemoteInstance&&) = delete;
        RemoteInstance& operator=(RemoteInstance const&) = delete;
        RemoteInstance& operator=(RemoteInstance&&) = delete;

        ~RemoteInstance() override
        {
            // Lowered before it is released, and given back only once the release is queued: the next
            // instance its slot is handed to is instantiated by a request, which sends every queued release
            // before it, so after the host let this one go. Nothing here waits for the host.
            _incarnation->clearInterrupt(_interruptSlot);
            _incarnation->release(
                proto::Release { .target = proto::ReleaseTarget::Instance, .id = _instanceId });
            _incarnation->interruptSlots().release(_interruptSlot);
        }

        [[nodiscard]] std::optional<ExportFunction> function(std::string_view name) const override
        {
            auto const exports = _module->exports();
            for (auto const ordinal: _callableExports)
            {
                auto const& description = exports[ordinal];
                if (description.name == name && description.signature.isAllI32())
                    return ExportFunction { .index = ordinal };
            }
            return std::nullopt;
        }

        [[nodiscard]] std::expected<void, CallFailure> call(ExportFunction function,
                                                            std::span<uint32_t> argv) override;

        [[nodiscard]] std::expected<std::span<std::byte>, MemoryFailure> memory(uint32_t address,
                                                                                uint32_t size) override
        {
            if (size == 0)
                return std::span<std::byte> {};
            if (!_incarnation->isAlive())
                return std::unexpected(MemoryFailure::HostCrashed);
            return _memory.view(address, size);
        }

        /// Wait-free: stores the sticky flag, then arms the backstop and raises the slot
        /// (HelperIncarnation::interrupt()). A call that enters its frame after the flag is stored sees it
        /// (call()), and one that entered it before is armed.
        void interrupt() noexcept override
        {
            _interrupt.store(InterruptState::Interrupted);
            _incarnation->interrupt(_instanceId, _interruptSlot);
        }

      private:
        /// @return The index of the calling thread's lane of the host; nothing if the thread has none.
        [[nodiscard]] std::optional<std::size_t> laneOfCurrentThread()
        {
            auto const lane = _module->runtime().kind().laneOfCurrentThread();
            return lane ? std::optional { *lane } : std::nullopt;
        }

        /// Kills the host: anything out of place on a channel means it can no longer be trusted.
        /// @return The failure every call into one of its instances fails with from now on.
        [[nodiscard]] std::unexpected<CallFailure> crashed() noexcept
        {
            _incarnation->markDead();
            return std::unexpected(CallFailure::HostCrashed);
        }

        /// Reads a range of the instance's memory on the calling thread's lane.
        /// @return The range's bytes; `OutOfBounds` if the host refused the range or the thread has no lane,
        ///         `HostCrashed` if the host died or broke the protocol, which kills it.
        [[nodiscard]] std::expected<std::vector<std::byte>, MemoryFailure> fetch(uint32_t address,
                                                                                 uint32_t size);

        /// Sends what was written to the instance's memory on @p lane, then drops every copy: the instance
        /// is about to run.
        [[nodiscard]] std::expected<void, CallFailure> flushTo(PduStream& lane);

        /// Runs the host call @p request asks for, and answers it on @p lane, whose watch is @p watch.
        [[nodiscard]] std::expected<void, CallFailure> serveHostCall(PduStream& lane,
                                                                     LaneWatch& watch,
                                                                     proto::CallHost const& request);

        std::shared_ptr<RemoteModule> _module;
        std::shared_ptr<HelperIncarnation> _incarnation;
        uint32_t _instanceId;
        uint32_t _interruptSlot; ///< The slot of the host's interrupt page it is interrupted by.
        std::vector<uint32_t> _callableExports; ///< The export ordinals the host offers to call.
        IHostContext& _context;
        std::atomic<InterruptState> _interrupt { InterruptState::Running };
        CopyPathMemory _memory;
    };

    /// @return The failure a call reports for what the host said it failed with: refused growth is not told
    ///         apart (MemoryGrowthReporting::Unreported), nor is a host's claim that it crashed believed.
    [[nodiscard]] CallFailure reportedFailure(CallFailure failure) noexcept
    {
        switch (failure)
        {
            case CallFailure::Trap:
            case CallFailure::Interrupted:
            case CallFailure::HostFailure: return failure;
            case CallFailure::MemoryGrowthRefused:
            case CallFailure::HostCrashed: return CallFailure::Trap;
        }
        return CallFailure::Trap;
    }

    std::expected<std::shared_ptr<IWasmModule>, ExtensionError> RemoteRuntime::load(
        ModuleIdentity identity, std::vector<std::byte> bytes)
    {
        auto slot = slotFor(identity.key);
        auto incarnation = slot->live();
        if (!incarnation)
            return std::unexpected(incarnation.error());
        auto loaded = loadInto(**incarnation, bytes);
        if (!loaded)
            return std::unexpected(loaded.error());
        return std::make_shared<RemoteModule>(shared_from_this(),
                                              std::move(slot),
                                              std::move(bytes),
                                              std::move(*incarnation),
                                              std::move(*loaded));
    }

    std::expected<std::unique_ptr<IWasmInstance>, ExtensionError> RemoteModule::instantiate(
        IHostContext& context)
    {
        auto const lock = std::scoped_lock { _mutex };
        auto incarnation = _slot->live();
        if (!incarnation)
            return std::unexpected(incarnation.error());
        auto& host = **incarnation;
        if (*incarnation != _loadedIn)
        {
            // The host it was loaded in died: loaded again in the new one, it must be the module it was. An
            // honest host describes the same bytes alike every time, so one that does not is not trusted, and
            // the module is adopted there only once it matched.
            auto reloaded = loadInto(host, _bytes);
            if (!reloaded)
                return std::unexpected(reloaded.error());
            if (reloaded->imports != _imports || reloaded->exports != _exports)
            {
                host.markDead();
                return std::unexpected(ExtensionError::InvalidComponent);
            }
            _loadedIn->release(proto::Release { .target = proto::ReleaseTarget::Module, .id = _moduleId });
            _loadedIn = *incarnation;
            _moduleId = reloaded->moduleId;
        }

        // Every live instance holds a slot of the host's interrupt page; with none left, the host takes no
        // more instances, which is no fault of the host's.
        auto const interruptSlot = host.interruptSlots().acquire();
        if (!interruptSlot)
            return std::unexpected(ExtensionError::RuntimeUnavailable);
        auto const refused = [&](ExtensionError error) {
            host.interruptSlots().release(*interruptSlot);
            return std::unexpected(error);
        };

        auto const instanceId = host.nextInstanceId();
        // No object yet: every host copies memory.
        auto reply = host.request(proto::Instantiate { .moduleId = _moduleId,
                                                       .instanceId = instanceId,
                                                       .interruptSlot = *interruptSlot,
                                                       .memory = {} },
                                  HostStep::Instantiate);
        if (!reply)
            return refused(ExtensionError::HostUnavailable);
        auto* const instantiated = std::get_if<proto::Instantiated>(&*reply);
        auto const callable = [&](uint32_t ordinal) {
            return ordinal < _exports.size() && _exports[ordinal].kind == ExternKind::Function
                   && _exports[ordinal].signature.parameters.size() <= proto::MaxCallValues
                   && _exports[ordinal].signature.results.size() <= proto::MaxCallValues;
        };
        if (!instantiated || instantiated->instanceId != instanceId
            || (instantiated->error && !isHostRefusal(*instantiated->error))
            || !std::ranges::all_of(instantiated->callableExports, callable))
        {
            host.markDead();
            return refused(ExtensionError::HostUnavailable);
        }
        if (instantiated->error)
            return refused(*instantiated->error);
        return std::make_unique<RemoteInstance>(shared_from_this(),
                                                std::move(*incarnation),
                                                instanceId,
                                                *interruptSlot,
                                                std::move(instantiated->callableExports),
                                                context);
    }

    std::expected<void, CallFailure> RemoteInstance::call(ExportFunction function, std::span<uint32_t> argv)
    {
        if (_interrupt.load() == InterruptState::Interrupted)
            return std::unexpected(CallFailure::Interrupted);
        if (!_incarnation->isAlive())
            return std::unexpected(CallFailure::HostCrashed);
        if (std::ranges::find(_callableExports, function.index) == _callableExports.end())
            return std::unexpected(CallFailure::Trap);
        auto const& signature = _module->exports()[function.index].signature;
        if (argv.size() < std::max(signature.parameters.size(), signature.results.size()))
            return std::unexpected(CallFailure::Trap);
        // A thread past the lanes the host serves cannot call (RemoteKindState logged it), nor can one that
        // was never prepared.
        auto const laneIndex = laneOfCurrentThread();
        if (!laneIndex)
            return std::unexpected(CallFailure::Trap);
        auto* const lane = &_incarnation->lane(*laneIndex);
        auto& watch = _incarnation->laneWatch(*laneIndex);

        // The frame is the backstop's to see before the flag is read again: an interrupt() racing this either
        // stored the flag before, and the call does not start, or finds the frame, and arms it.
        auto const frame = LaneFrame { watch, _instanceId };
        if (_interrupt.load() == InterruptState::Interrupted)
            return std::unexpected(CallFailure::Interrupted);
        if (auto const flushed = flushTo(*lane); !flushed)
            return flushed;
        auto const arguments = argv.first(signature.parameters.size());
        if (!lane->send(proto::Call { .instanceId = _instanceId,
                                      .exportOrdinal = function.index,
                                      .argv = { arguments.begin(), arguments.end() } }))
            return crashed();
        while (true)
        {
            auto reply = lane->receive();
            if (!reply)
                return crashed();
            if (auto const* const hostCall = std::get_if<proto::CallHost>(&*reply))
            {
                if (auto const served = serveHostCall(*lane, watch, *hostCall); !served)
                    return served;
                continue;
            }
            auto* const result = std::get_if<proto::CallResult>(&*reply);
            if (!result)
                return crashed();
            // A call that failed once its instance was interrupted failed for the interrupt, whatever the
            // host makes of a host call refused for it.
            if (result->failure)
                return std::unexpected(_interrupt.load() == InterruptState::Interrupted
                                           ? CallFailure::Interrupted
                                           : reportedFailure(*result->failure));
            if (result->argv.size() != signature.results.size())
                return crashed();
            std::ranges::copy(result->argv, argv.begin());
            return {};
        }
    }

    std::expected<void, CallFailure> RemoteInstance::serveHostCall(PduStream& lane,
                                                                   LaneWatch& watch,
                                                                   proto::CallHost const& request)
    {
        auto const functions = _module->runtime().functions();
        if (request.functionIndex >= functions.size()
            || request.arguments.size() != functions[request.functionIndex].parameterCount)
            return crashed();
        // An interrupted instance's host calls are refused, not served: a host that runs on past an interrupt
        // cannot keep its grace paused on the parent's time.
        if (_interrupt.load() == InterruptState::Interrupted)
        {
            if (!lane.send(proto::HostResult { .error = ExtensionError::DeadlineExceeded }))
                return crashed();
            return {};
        }
        // The parent's own time: only the host function. Answering it is the host's again, as a host that
        // stops reading must not hold the lane past the grace.
        auto const result = [&] {
            auto const parked = HostCallFrame { watch };
            return _context.callHost(request.functionIndex, request.arguments);
        }();
        // What the host function wrote reaches the guest before it runs on.
        if (auto const flushed = flushTo(lane); !flushed)
            return flushed;
        auto const error = result ? std::nullopt : std::optional { result.error() };
        if (!lane.send(proto::HostResult { .error = error }))
            return crashed();
        return {};
    }

    std::expected<void, CallFailure> RemoteInstance::flushTo(PduStream& lane)
    {
        auto const writes = _memory.flush();
        _memory.clear();
        for (auto const& write: writes)
            if (!lane.send(write))
                return crashed();
        return {};
    }

    std::expected<std::vector<std::byte>, MemoryFailure> RemoteInstance::fetch(uint32_t address,
                                                                               uint32_t size)
    {
        // A thread that cannot call cannot read either: as a call from it traps, its read is refused.
        auto const laneIndex = laneOfCurrentThread();
        if (!laneIndex)
            return std::unexpected(MemoryFailure::OutOfBounds);
        auto* const lane = &_incarnation->lane(*laneIndex);
        // A read is a frame of its own: a host that stalls in its answer is the backstop's, once interrupted.
        // An instance interrupted before the read began -- a deadline that ran out while a call's results
        // were lifted -- has its read armed here, as interrupt() found no frame of it then.
        auto const frame = LaneFrame { _incarnation->laneWatch(*laneIndex), _instanceId };
        if (_interrupt.load() == InterruptState::Interrupted)
            _incarnation->interrupt(_instanceId, _interruptSlot);
        if (!lane->send(proto::MemoryRead { .instanceId = _instanceId, .address = address, .size = size }))
        {
            _incarnation->markDead();
            return std::unexpected(MemoryFailure::HostCrashed);
        }
        auto reply = lane->receive();
        auto* const data = reply ? std::get_if<proto::MemoryData>(&*reply) : nullptr;
        if (!data || (data->status == proto::MemoryStatus::Ok && data->bytes.size() != size))
        {
            _incarnation->markDead();
            return std::unexpected(MemoryFailure::HostCrashed);
        }
        if (data->status != proto::MemoryStatus::Ok)
            return std::unexpected(MemoryFailure::OutOfBounds);
        return std::move(data->bytes);
    }

    /// Runs every module in hosts the connector starts.
    class RemoteBackend final: public IWasmBackend
    {
      public:
        RemoteBackend(std::shared_ptr<RemoteKindState> kind, BackendConfig config):
            _kind { std::move(kind) }, _config { std::move(config) }
        {
        }

        RemoteBackend(RemoteBackend const&) = delete;
        RemoteBackend(RemoteBackend&&) = delete;
        RemoteBackend& operator=(RemoteBackend const&) = delete;
        RemoteBackend& operator=(RemoteBackend&&) = delete;

        /// Lets every host it started go, and returns: a thread still using one fails `HostUnavailable`
        /// rather than keep this waiting.
        ~RemoteBackend() override { _roster->close(); }

        [[nodiscard]] std::expected<std::shared_ptr<IWasmRuntime>, ExtensionError> createRuntime(
            RuntimeLimits limits, std::span<HostFunction const> hostFunctions) override
        {
            // What no host accepts is refused here, as an in-process backend refuses it, rather than when the
            // first module is loaded.
            if (limits.stackBytes > proto::MaxRuntimeStackBytes || limits.tableElements > MaxTableElements
                || hostFunctions.size() > proto::MaxHostFunctions
                || std::ranges::any_of(hostFunctions, [](HostFunction const& function) {
                       return function.parameterCount > MaxHostParameters;
                   }))
                return std::unexpected(ExtensionError::RuntimeUnavailable);
            auto create = proto::CreateRuntime { .runtimeId = HelperIncarnation::RuntimeId,
                                                 .memoryBytes = limits.memoryBytes,
                                                 .stackBytes = limits.stackBytes,
                                                 .tableElements = limits.tableElements,
                                                 .functions = {} };
            create.functions.reserve(hostFunctions.size());
            for (auto const& function: hostFunctions)
                create.functions.push_back(
                    proto::WireHostFunction { .module = std::string { function.module },
                                              .name = std::string { function.name },
                                              .parameterCount = function.parameterCount });
            return std::make_shared<RemoteRuntime>(
                _kind,
                _roster,
                _config.connector,
                _refusals,
                HelperPolicy { .timeouts = _config.timeouts, .lockdown = _config.lockdown },
                limits,
                std::move(create));
        }

        [[nodiscard]] std::expected<std::unique_ptr<IThreadGuard>, ExtensionError> enterThread() override
        {
            return _kind->enterThread();
        }

        [[nodiscard]] bool isCurrentThreadPrepared() const override
        {
            return _kind->isCurrentThreadPrepared();
        }

        [[nodiscard]] uint64_t runtimeGeneration() const noexcept override { return _kind->generation(); }

        [[nodiscard]] BackendCapabilities capabilities() const noexcept override
        {
            // Which engine a host runs is not known before one is started; refused growth is reported as a
            // trap, which every engine can do.
            return BackendCapabilities { .memoryGrowth = MemoryGrowthReporting::Unreported };
        }

      private:
        std::shared_ptr<RemoteKindState> _kind;
        BackendConfig _config;
        std::shared_ptr<IncarnationRoster> _roster = std::make_shared<IncarnationRoster>();
        /// Why its hosts were refused, each reason logged once for the backend.
        std::shared_ptr<HostRefusalLog> _refusals = std::make_shared<HostRefusalLog>(errorLogWriter());
    };
} // namespace

std::unique_ptr<IWasmBackend> makeRemoteBackend(std::shared_ptr<RemoteKindState> kind, BackendConfig config)
{
    return std::make_unique<RemoteBackend>(std::move(kind), std::move(config));
}

} // namespace vtextension::wasm::remote
