// SPDX-License-Identifier: Apache-2.0
//
// The Wasmtime backend's instances: one store each, held to the runtime's limits.
#include <core/Assert.hpp>
#include <core/Utils.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <wasmtime.h>

#include <vtextension/wasm/wasmtime/WasmtimeHandles.hpp>
#include <vtextension/wasm/wasmtime/WasmtimeRuntime.hpp>

namespace vtextension::wasm::wasmtime
{

namespace
{
    /// The WebAssembly page size, in bytes.
    constexpr auto PageBytes = uint64_t { 64 } * 1024;

    /// How many values a call passes each way without allocating: the Canonical ABI's limit on flat
    /// parameters, which every function of the extension world keeps to.
    constexpr auto InlineValues = std::size_t { 16 };

    /// The values of one direction of a call: @p count of them, in @p inlineValues when they fit, else in
    /// @p wideValues.
    /// @return Where the @p count values are.
    [[nodiscard]] std::span<wasmtime_val_t> valuesFor(std::size_t count,
                                                      std::array<wasmtime_val_t, InlineValues>& inlineValues,
                                                      std::vector<wasmtime_val_t>& wideValues)
    {
        if (count <= inlineValues.size())
            return std::span { inlineValues }.first(count);
        wideValues.resize(count);
        return wideValues;
    }

    /// One function export of an instance, looked up once.
    struct FunctionEntry
    {
        std::string name;            ///< The export's name.
        wasmtime_func_t function {}; ///< Wasmtime's handle on it, in the instance's store.
        uint32_t parameterCount = 0; ///< How many i32 parameters it takes.
        uint32_t resultCount = 0;    ///< How many i32 results it returns.
    };

    /// @return Whether every type of @p types is an i32: the cells IWasmInstance::call() passes.
    [[nodiscard]] bool allI32(std::span<ValueType const> types) noexcept
    {
        return std::ranges::all_of(types, [](ValueType type) { return type == ValueType::I32; });
    }

    /// How far beyond the current epoch a guest's deadline lies: one tick. Only an interrupt moves the
    /// epoch, so a guest that is never interrupted never reaches it and nothing ever wakes to poll it;
    /// one that is reaches it at its next epoch check.
    constexpr auto DeadlineTicks = uint64_t { 1 };

    /// Sets the deadline of the guest of @p context, before every entry into guest code. A store's
    /// deadline starts at epoch 0, which the engine's epoch has always reached.
    void armEpochDeadline(wasmtime_context_t* context) noexcept
    {
        wasmtime_context_set_epoch_deadline(context, DeadlineTicks);
    }

    /// Wasmtime's epoch deadline callback: called on the thread of a running guest whose deadline the
    /// epoch reached. The epoch is the engine's, shared by every instance of the runtime, so reaching
    /// the deadline says only that some instance was interrupted: this one's own flag decides.
    /// Called from Wasmtime's Rust code, which an exception must not unwind through: noexcept.
    ///
    /// A known window, accepted: when this callback finds the flag not set and returns, Wasmtime sets the
    /// new deadline one tick past the epoch *as it reads it then*, after the flag was read. An interrupt()
    /// of this instance landing between the flag read and that re-read moves the epoch the new deadline is
    /// then counted from, so the guest is not woken again: the interrupt is lost until another one moves
    /// the epoch. It cannot be closed from here: the callback only hands back a delta relative to an epoch
    /// Wasmtime reads after it returns, and neither it nor wasmtime_context_set_epoch_deadline sets an
    /// absolute deadline or compares the epoch it counts from. It takes two instances sharing an engine
    /// (another's interrupt woke this guest) and an interrupt landing within a few instructions. The
    /// in-process Wasmtime backend serves tests; in production the guest runs in a helper process, and the
    /// supervisor's grace timer kills a helper whose interrupted call does not return.
    /// @param env The instance's StoreData.
    /// @return An error ending the call if the instance was interrupted; otherwise null, with the deadline
    ///         moved one tick past the current epoch.
    wasmtime_error_t* onEpochDeadline(wasmtime_context_t* /*context*/,
                                      void* env,
                                      uint64_t* deadlineTicks,
                                      wasmtime_update_deadline_kind_t* updateKind) noexcept
    {
        auto& data = *static_cast<StoreData*>(env);
        data.kind.countEpochCallback();
        // Pairs with the fence in interrupt(): the epoch this guest saw moved after the flag was stored.
        std::atomic_thread_fence(std::memory_order_acquire);
        if (data.interrupt.load(std::memory_order_relaxed) == InterruptState::Interrupted)
            return wasmtime_error_new("interrupted");
        *deadlineTicks = DeadlineTicks;
        *updateKind = WASMTIME_UPDATE_DEADLINE_CONTINUE;
        return nullptr;
    }

    /// One module instance in its own store.
    ///
    /// The store's data is the instance's StoreData: that is how a host function call made from inside the
    /// instance finds its IHostContext, and how a failing one is told apart from a trap of the guest's.
    class WasmtimeInstance final: public IWasmInstance
    {
        struct PrivateTag
        {
        };

      public:
        /// @see WasmtimeModule::instantiate
        [[nodiscard]] static std::expected<std::unique_ptr<IWasmInstance>, ExtensionError> create(
            std::shared_ptr<WasmtimeModule const> owner,
            WasmtimeRuntime const& runtime,
            wasmtime_module_t const* module,
            std::span<ExportDescription const> exports,
            uint32_t largestInitialPages,
            IHostContext& context);

        /// Use create(); public only so that std::make_unique can reach it.
        WasmtimeInstance(PrivateTag,
                         std::shared_ptr<WasmtimeModule const> owner,
                         std::unique_ptr<StoreData> data,
                         detail::StoreHandle store,
                         std::vector<FunctionEntry> functions,
                         std::optional<wasmtime_memory_t> memory,
                         wasm_engine_t const* engine) noexcept:
            _owner { std::move(owner) },
            _data { std::move(data) },
            _store { std::move(store) },
            _functions { std::move(functions) },
            _memory { memory },
            _engine { engine }
        {
        }

        ~WasmtimeInstance() override = default;
        WasmtimeInstance(WasmtimeInstance const&) = delete;
        WasmtimeInstance(WasmtimeInstance&&) = delete;
        WasmtimeInstance& operator=(WasmtimeInstance const&) = delete;
        WasmtimeInstance& operator=(WasmtimeInstance&&) = delete;

        /// Only function exports taking and returning nothing but i32s are found: they are what call()
        /// passes cells to, and every function of the extension world is one.
        [[nodiscard]] std::optional<ExportFunction> function(std::string_view name) const override
        {
            auto const entry = std::ranges::find(_functions, name, &FunctionEntry::name);
            if (entry == _functions.end())
                return std::nullopt;
            return ExportFunction { .index = static_cast<uint32_t>(entry - _functions.begin()) };
        }

        [[nodiscard]] std::expected<void, CallFailure> call(ExportFunction function,
                                                            std::span<uint32_t> argv) override
        {
            Require(function.index < _functions.size());
            auto const& entry = _functions[function.index];
            Require(argv.size() >= std::max(entry.parameterCount, entry.resultCount));

            // The deadline goes first, the interrupt check after it: an interrupt landing between the two is
            // seen by the check, and one landing after the check moves the epoch past the deadline, which
            // stops the guest at its first epoch check. The fence pairs with interrupt()'s: if setting the
            // deadline read an epoch an interrupt moved, the check sees that interrupt's flag.
            auto* const context = storeContext();
            armEpochDeadline(context);
            std::atomic_thread_fence(std::memory_order_acquire);
            // The interrupt is sticky: once requested, no call enters the guest again.
            if (_data->interrupt.load(std::memory_order_relaxed) == InterruptState::Interrupted)
                return interrupted(context);

            // A host function may call back into the instance (to allocate in guest memory): what the
            // outermost call records is kept until it returns.
            if (_callDepth == 0)
                _data->hostFailure = std::nullopt;

            auto inlineArguments = std::array<wasmtime_val_t, InlineValues> {};
            auto wideArguments = std::vector<wasmtime_val_t> {};
            auto const arguments = valuesFor(entry.parameterCount, inlineArguments, wideArguments);
            for (auto const [index, argument]: core::views::enumerate(arguments))
            {
                argument.kind = WASMTIME_I32;
                argument.of.i32 = static_cast<int32_t>(argv[static_cast<std::size_t>(index)]);
            }
            auto inlineResults = std::array<wasmtime_val_t, InlineValues> {};
            auto wideResults = std::vector<wasmtime_val_t> {};
            auto const results = valuesFor(entry.resultCount, inlineResults, wideResults);

            auto* rawTrap = static_cast<wasm_trap_t*>(nullptr);
            ++_callDepth;
            auto const error = detail::ErrorHandle { wasmtime_func_call(context,
                                                                        &entry.function,
                                                                        arguments.data(),
                                                                        arguments.size(),
                                                                        results.data(),
                                                                        results.size(),
                                                                        &rawTrap) };
            --_callDepth;
            auto const trap = detail::TrapHandle { rawTrap };
            auto const failed = error || trap ? std::optional { failure() } : std::nullopt;
            // A nested call reports its host failure through its own result: the host function that made
            // it decides what becomes of it, and the call it is nested in must not report it again.
            if (_callDepth > 0)
                _data->hostFailure = std::nullopt;
            if (failed == CallFailure::Interrupted)
                return interrupted(context);
            if (failed)
                return std::unexpected(*failed);

            std::ranges::transform(results, argv.begin(), [](wasmtime_val_t const& result) {
                return static_cast<uint32_t>(result.of.i32);
            });
            return {};
        }

        [[nodiscard]] std::optional<std::span<std::byte>> memory(uint32_t address, uint32_t size) override
        {
            if (size == 0)
                return std::span<std::byte> {};
            if (!_memory)
                return std::nullopt;
            auto* const context = storeContext();
            auto const bytes = wasmtime_memory_data_size(context, &*_memory);
            if (uint64_t { address } + size > bytes)
                return std::nullopt;
            auto const whole =
                std::span<std::byte> { reinterpret_cast<std::byte*>(wasmtime_memory_data(context, &*_memory)),
                                       bytes };
            return whole.subspan(address, size);
        }

        /// Wait-free, from any thread, before, during or after a call: stores the flag, then moves the
        /// engine's epoch past the deadline of every guest of the runtime running now. Each of them wakes
        /// once (onEpochDeadline); only this instance's stops. One landing just as this instance's guest
        /// was woken by another instance's interrupt can be missed (the window documented at
        /// onEpochDeadline); the helper process's supervisor kills a call that then never returns.
        void interrupt() noexcept override
        {
            _data->interrupt.store(InterruptState::Interrupted, std::memory_order_relaxed);
            // Orders the flag before the epoch increment, for whoever reads the moved epoch.
            std::atomic_thread_fence(std::memory_order_release);
            wasmtime_engine_increment_epoch(_engine);
        }

      private:
        /// Ends a call as interrupted. A nested call also makes the deadline of the call it is nested in
        /// due at once: the store has one deadline, which this call set one tick past an epoch the
        /// interrupt may already have moved, and the host function may absorb this failure and return
        /// into a guest that would then run on.
        /// @param context The context the call runs in.
        /// @return The failure `Interrupted`.
        [[nodiscard]] std::unexpected<CallFailure> interrupted(wasmtime_context_t* context) const noexcept
        {
            if (_callDepth > 0)
                wasmtime_context_set_epoch_deadline(context, 0);
            return std::unexpected(CallFailure::Interrupted);
        }

        /// @return The context the store is reached through: the caller's while a host function call runs,
        ///         the store's own otherwise.
        [[nodiscard]] wasmtime_context_t* storeContext() const noexcept
        {
            if (_data->callerContext)
                return _data->callerContext;
            return wasmtime_store_context(_store.get());
        }

        /// @return Why the call that just failed did. Decided by what the instance recorded, never by a
        ///         trap's message: an interrupt ends a call only through onEpochDeadline, which reads the
        ///         flag this reads.
        [[nodiscard]] CallFailure failure() const noexcept
        {
            if (_data->interrupt.load(std::memory_order_acquire) == InterruptState::Interrupted)
                return CallFailure::Interrupted;
            if (_data->hostFailure)
                return CallFailure::HostFailure;
            return CallFailure::Trap;
        }

        std::shared_ptr<WasmtimeModule const> _owner; ///< Declared first: released after the instance.
        std::unique_ptr<StoreData> _data;             ///< The store's data; outlives the store.
        detail::StoreHandle _store;
        std::vector<FunctionEntry> _functions;
        std::optional<wasmtime_memory_t> _memory; ///< The first memory the instance exports, if any.
        wasm_engine_t const* _engine;             ///< The runtime's engine, whose epoch interrupt() moves.
        uint32_t _callDepth = 0;                  ///< Calls running, nested ones included.
    };

    std::expected<std::unique_ptr<IWasmInstance>, ExtensionError> WasmtimeInstance::create(
        std::shared_ptr<WasmtimeModule const> owner,
        WasmtimeRuntime const& runtime,
        wasmtime_module_t const* module,
        std::span<ExportDescription const> exports,
        uint32_t largestInitialPages,
        IHostContext& context)
    {
        // Checked before instantiating, exported memories and others alike: the limiter's refusal would
        // fail the instantiation as an error indistinguishable from any other.
        auto const capBytes = runtime.limits().memoryBytes;
        if (largestInitialPages * PageBytes > capBytes)
            return std::unexpected(ExtensionError::OutOfMemory);

        auto data = std::make_unique<StoreData>(context, runtime.wasiEnvironment(), runtime.kind());
        auto store = detail::StoreHandle { wasmtime_store_new(runtime.engine(), data.get(), nullptr) };
        if (!store)
            return std::unexpected(ExtensionError::RuntimeUnavailable);
        // Before anything runs in the store, a start function included: every deadline is one an interrupt
        // of any instance of the runtime moves the epoch past.
        wasmtime_store_epoch_deadline_callback(store.get(), &onEpochDeadline, data.get(), nullptr);
        // Memory grows up to the cap and no further (memory.grow returns -1); one instance and one memory
        // per store.
        auto const memoryCap = static_cast<int64_t>(
            std::min<uint64_t>(capBytes, static_cast<uint64_t>(std::numeric_limits<int64_t>::max())));
        wasmtime_store_limiter(store.get(), memoryCap, -1, 1, -1, 1);

        auto* const storeContext = wasmtime_store_context(store.get());
        // A start function runs during instantiation.
        armEpochDeadline(storeContext);
        auto instance = wasmtime_instance_t {};
        auto* rawTrap = static_cast<wasm_trap_t*>(nullptr);
        auto const error = detail::ErrorHandle { wasmtime_linker_instantiate(
            runtime.linker(), storeContext, module, &instance, &rawTrap) };
        auto const trap = detail::TrapHandle { rawTrap };
        if (error || trap)
            return std::unexpected(ExtensionError::InvalidComponent);

        auto functions = std::vector<FunctionEntry> {};
        auto memory = std::optional<wasmtime_memory_t> {};
        for (auto const& entry: exports)
        {
            auto item = wasmtime_extern_t {};
            if (!wasmtime_instance_export_get(
                    storeContext, &instance, entry.name.data(), entry.name.size(), &item))
                continue;
            // A function or a memory is a plain handle into the store, kept by value; anything else is
            // not kept, and released.
            if (item.kind == WASMTIME_EXTERN_FUNC)
            {
                if (allI32(entry.signature.parameters) && allI32(entry.signature.results))
                    functions.push_back(FunctionEntry {
                        .name = entry.name,
                        .function = item.of.func,
                        .parameterCount = static_cast<uint32_t>(entry.signature.parameters.size()),
                        .resultCount = static_cast<uint32_t>(entry.signature.results.size()) });
            }
            else if (item.kind == WASMTIME_EXTERN_MEMORY)
            {
                if (!memory)
                    memory = item.of.memory;
            }
            else
                wasmtime_extern_delete(&item);
        }

        return std::make_unique<WasmtimeInstance>(PrivateTag {},
                                                  std::move(owner),
                                                  std::move(data),
                                                  std::move(store),
                                                  std::move(functions),
                                                  memory,
                                                  runtime.engine());
    }
} // namespace

std::expected<std::unique_ptr<IWasmInstance>, ExtensionError> WasmtimeModule::instantiate(
    IHostContext& context)
{
    // The guest's `_initialize` is never run here: it stays an ordinary export, the caller's to call.
    return WasmtimeInstance::create(
        shared_from_this(), *_runtime, _module.get(), _exports, _largestInitialPages, context);
}

} // namespace vtextension::wasm::wasmtime
