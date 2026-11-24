// SPDX-License-Identifier: Apache-2.0
//
// The Wasmtime backend's instances: one store each, held to the runtime's limits.
#include <core/Assert.hpp>
#include <core/Utils.hpp>

#include <algorithm>
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

    /// One function export of an instance, looked up once.
    struct FunctionEntry
    {
        std::string name;            ///< The export's name.
        wasmtime_func_t function {}; ///< Wasmtime's handle on it, in the instance's store.
        uint32_t parameterCount = 0; ///< How many i32 parameters it takes.
        uint32_t resultCount = 0;    ///< How many i32 results it returns.
    };

    /// Whether IWasmInstance::interrupt() was called.
    enum class Interruption : uint8_t
    {
        None,
        Requested,
    };

    /// @return Whether every type of @p types is an i32: the cells IWasmInstance::call() passes.
    [[nodiscard]] bool allI32(std::span<ValueType const> types) noexcept
    {
        return std::ranges::all_of(types, [](ValueType type) { return type == ValueType::I32; });
    }

    /// Lets the guest of @p context run, before every entry into guest code.
    ///
    /// A store's epoch deadline starts at 0 ticks, so with epoch interruption on, a guest would trap at its
    /// first epoch check. Until a deadline is what interrupt() stops a running call with, the deadline is
    /// set as far out as it goes: an interrupt then stops only calls not yet entered.
    void armEpochDeadline(wasmtime_context_t* context) noexcept
    {
        wasmtime_context_set_epoch_deadline(context, std::numeric_limits<uint64_t>::max());
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
            IHostContext& context);

        /// Use create(); public only so that std::make_unique can reach it.
        WasmtimeInstance(PrivateTag,
                         std::shared_ptr<WasmtimeModule const> owner,
                         std::unique_ptr<StoreData> data,
                         detail::StoreHandle store,
                         std::vector<FunctionEntry> functions,
                         std::optional<wasmtime_memory_t> memory) noexcept:
            _owner { std::move(owner) },
            _data { std::move(data) },
            _store { std::move(store) },
            _functions { std::move(functions) },
            _memory { memory }
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

            // The interrupt is sticky: once requested, no call enters the guest again.
            if (_interruption.load(std::memory_order_acquire) == Interruption::Requested)
                return std::unexpected(CallFailure::Interrupted);

            // A host function may call back into the instance (to allocate in guest memory): what the
            // outermost call records is kept until it returns.
            if (_callDepth == 0)
                _data->hostFailure = std::nullopt;

            auto arguments = std::vector<wasmtime_val_t>(entry.parameterCount);
            for (auto const [index, argument]: core::views::enumerate(arguments))
            {
                argument.kind = WASMTIME_I32;
                argument.of.i32 = static_cast<int32_t>(argv[static_cast<std::size_t>(index)]);
            }
            auto results = std::vector<wasmtime_val_t>(entry.resultCount);

            auto* const context = wasmtime_store_context(_store.get());
            armEpochDeadline(context);
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
            auto const failed = error || trap ? std::optional { failure(trap.get()) } : std::nullopt;
            // A nested call reports its host failure through its own result: the host function that made
            // it decides what becomes of it, and the call it is nested in must not report it again.
            if (_callDepth > 0)
                _data->hostFailure = std::nullopt;
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
            auto* const context = wasmtime_store_context(_store.get());
            auto const bytes = wasmtime_memory_data_size(context, &*_memory);
            if (uint64_t { address } + size > bytes)
                return std::nullopt;
            auto const whole =
                std::span<std::byte> { reinterpret_cast<std::byte*>(wasmtime_memory_data(context, &*_memory)),
                                       bytes };
            return whole.subspan(address, size);
        }

        void interrupt() noexcept override
        {
            // Final for every call not yet entered. Stopping one already running takes an epoch deadline
            // near enough to be reached (armEpochDeadline).
            _interruption.store(Interruption::Requested, std::memory_order_release);
        }

      private:
        /// @param trap The trap the call failed with; null if Wasmtime refused the call itself.
        /// @return Why the call that just failed did. Decided by what the instance recorded and by the
        ///         trap's code, never by its message.
        [[nodiscard]] CallFailure failure(wasm_trap_t const* trap) const noexcept
        {
            if (_interruption.load(std::memory_order_acquire) == Interruption::Requested)
                return CallFailure::Interrupted;
            if (_data->hostFailure)
                return CallFailure::HostFailure;
            auto code = wasmtime_trap_code_t {};
            if (trap && wasmtime_trap_code(trap, &code) && code == WASMTIME_TRAP_CODE_INTERRUPT)
                return CallFailure::Interrupted;
            return CallFailure::Trap;
        }

        std::shared_ptr<WasmtimeModule const> _owner; ///< Declared first: released after the instance.
        std::unique_ptr<StoreData> _data;             ///< The store's data; outlives the store.
        detail::StoreHandle _store;
        std::vector<FunctionEntry> _functions;
        std::optional<wasmtime_memory_t> _memory; ///< The first memory the instance exports, if any.
        std::atomic<Interruption> _interruption = Interruption::None; ///< Written from any thread.
        uint32_t _callDepth = 0; ///< Calls running, nested ones included.
    };

    std::expected<std::unique_ptr<IWasmInstance>, ExtensionError> WasmtimeInstance::create(
        std::shared_ptr<WasmtimeModule const> owner,
        WasmtimeRuntime const& runtime,
        wasmtime_module_t const* module,
        std::span<ExportDescription const> exports,
        IHostContext& context)
    {
        auto const capBytes = runtime.limits().memoryBytes;
        auto const exceedsCap = [&](ExportDescription const& entry) {
            return entry.kind == ExternKind::Memory && entry.initialPages * PageBytes > capBytes;
        };
        if (std::ranges::any_of(exports, exceedsCap))
            return std::unexpected(ExtensionError::OutOfMemory);

        auto data = std::make_unique<StoreData>(StoreData {
            .context = context, .wasiEnvironment = runtime.wasiEnvironment(), .hostFailure = std::nullopt });
        auto store = detail::StoreHandle { wasmtime_store_new(runtime.engine(), data.get(), nullptr) };
        if (!store)
            return std::unexpected(ExtensionError::RuntimeUnavailable);
        // Memory grows up to the cap and no further (memory.grow returns -1); one instance and one memory
        // per store. A memory the exports did not show and whose initial size exceeds the cap fails the
        // instantiation below.
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

        return std::make_unique<WasmtimeInstance>(
            PrivateTag {}, std::move(owner), std::move(data), std::move(store), std::move(functions), memory);
    }
} // namespace

std::expected<std::unique_ptr<IWasmInstance>, ExtensionError> WasmtimeModule::instantiate(
    IHostContext& context)
{
    // The guest's `_initialize` is never run here: it stays an ordinary export, the caller's to call.
    return WasmtimeInstance::create(shared_from_this(), *_runtime, _module.get(), _exports, context);
}

} // namespace vtextension::wasm::wasmtime
