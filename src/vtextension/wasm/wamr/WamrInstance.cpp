// SPDX-License-Identifier: Apache-2.0
//
// The WAMR backend's instances, the trampolines WAMR calls host functions through, and the handler of
// WAMR's memory-growth callback.
#include <core/Assert.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <wasm_export.h>

#include <vtextension/wasm/wamr/WamrHandles.hpp>
#include <vtextension/wasm/wamr/WamrRuntime.hpp>

namespace vtextension::wasm::wamr
{

namespace
{
    /// The WebAssembly page size, in bytes.
    constexpr auto PageBytes = std::size_t { 64 } * 1024;

    /// The most pages a 32-bit linear memory can have.
    constexpr auto MaxPages = std::size_t { 65536 };

    /// The exception a host function call is stopped with when its host function failed.
    constexpr auto HostFailureException = "extension host function failed";

    /// One function export of an instance, looked up once.
    struct FunctionEntry
    {
        std::string name;                        ///< The export's name.
        wasm_function_inst_t function = nullptr; ///< WAMR's handle on it.
        uint32_t parameterCount = 0;             ///< How many parameter cells it takes.
        uint32_t resultCount = 0;                ///< How many result cells it returns.
    };

    /// Whether IWasmInstance::interrupt() was called.
    enum class Interruption : uint8_t
    {
        None,
        Requested,
    };

    /// @return WAMR's cap on the pages of an instance's memory under @p limits; 0 if not even one page
    ///         fits (which WAMR cannot express: to it, 0 means no cap).
    [[nodiscard]] uint32_t capPagesOf(RuntimeLimits const& limits) noexcept
    {
        return static_cast<uint32_t>(std::min(limits.memoryBytes / PageBytes, MaxPages));
    }

    /// @return @p bytes as a 32-bit size WAMR takes, saturated.
    [[nodiscard]] uint32_t saturated32(std::size_t bytes) noexcept
    {
        return static_cast<uint32_t>(std::min<std::size_t>(bytes, std::numeric_limits<uint32_t>::max()));
    }

    /// One module instance in WAMR, with the execution environment its calls run in.
    ///
    /// The execution environment's user data points back here: that is how a host function trampoline and
    /// the memory-growth handler find the instance. The module instance's custom data is left alone (and
    /// null).
    class WamrInstance final: public IWasmInstance
    {
        struct PrivateTag
        {
        };

      public:
        /// @see instantiateModule
        [[nodiscard]] static std::expected<std::unique_ptr<IWasmInstance>, ExtensionError> create(
            std::shared_ptr<IWasmModule const> owner,
            WASMModuleCommon* module,
            RuntimeLimits const& limits,
            std::span<ExportDescription const> exports,
            IHostContext& context);

        /// Use create(); public only so that std::make_unique can reach it.
        WamrInstance(PrivateTag,
                     std::shared_ptr<IWasmModule const> owner,
                     IHostContext& context,
                     detail::InstanceHandle instance,
                     detail::ExecEnvHandle execEnv,
                     std::vector<FunctionEntry> functions) noexcept:
            _owner { std::move(owner) },
            _context { context },
            _instance { std::move(instance) },
            _execEnv { std::move(execEnv) },
            _functions { std::move(functions) }
        {
            wasm_runtime_set_user_data(_execEnv.get(), this);
        }

        ~WamrInstance() override = default;
        WamrInstance(WamrInstance const&) = delete;
        WamrInstance(WamrInstance&&) = delete;
        WamrInstance& operator=(WamrInstance const&) = delete;
        WamrInstance& operator=(WamrInstance&&) = delete;

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

            // The interrupt is sticky: once requested, no call enters the guest again. A request landing
            // after this check finds WAMR's exception set, which stops the call before it gets far.
            if (_interruption.load(std::memory_order_acquire) == Interruption::Requested)
                return std::unexpected(CallFailure::Interrupted);

            // A host function may call back into the instance (to allocate in guest memory): what the
            // outermost call records is kept until it returns.
            if (_callDepth == 0)
                _fault = std::nullopt;
            ++_callDepth;
            auto const returned =
                wasm_runtime_call_wasm(_execEnv.get(), entry.function, entry.parameterCount, argv.data());
            --_callDepth;
            if (returned)
                return {};
            return std::unexpected(failure());
        }

        [[nodiscard]] std::optional<std::span<std::byte>> memory(uint32_t address, uint32_t size) override
        {
            if (size == 0)
                return std::span<std::byte> {};
            auto* const memory = wasm_runtime_get_default_memory(_instance.get());
            if (!memory)
                return std::nullopt;
            auto const bytes =
                wasm_memory_get_cur_page_count(memory) * wasm_memory_get_bytes_per_page(memory);
            if (uint64_t { address } + size > bytes)
                return std::nullopt;
            auto const whole =
                std::span<std::byte> { static_cast<std::byte*>(wasm_memory_get_base_address(memory)),
                                       static_cast<std::size_t>(bytes) };
            return whole.subspan(address, size);
        }

        void interrupt() noexcept override
        {
            _interruption.store(Interruption::Requested, std::memory_order_release);
            wasm_runtime_terminate(_instance.get());
        }

        /// Runs a host function on behalf of the guest; called by its trampoline, on the calling thread.
        /// @param functionIndex The host function's index in the runtime's table.
        /// @param arguments Its flat arguments.
        void callHost(std::size_t functionIndex, std::span<uint32_t const> arguments) noexcept
        {
            if (_context.callHost(functionIndex, arguments))
                return;
            record(CallFailure::HostFailure);
            wasm_runtime_set_exception(_instance.get(), HostFailureException);
        }

        /// Records that the running call's memory refused to grow; called by the memory-growth handler.
        void refuseGrowth() noexcept { record(CallFailure::MemoryGrowthRefused); }

      private:
        /// Records why the running call is failing. A host function's failure outranks a refused growth:
        /// the growth a host function's allocation needed is what made it fail.
        void record(CallFailure fault) noexcept
        {
            if (!_fault || fault == CallFailure::HostFailure)
                _fault = fault;
        }

        /// @return Why the call that just failed did.
        [[nodiscard]] CallFailure failure() const noexcept
        {
            if (_interruption.load(std::memory_order_acquire) == Interruption::Requested)
                return CallFailure::Interrupted;
            return _fault.value_or(CallFailure::Trap);
        }

        std::shared_ptr<IWasmModule const> _owner; ///< Declared first: released after the instance.
        IHostContext& _context;
        detail::InstanceHandle _instance;
        detail::ExecEnvHandle _execEnv; ///< Destroyed before the instance it runs in.
        std::vector<FunctionEntry> _functions;
        std::atomic<Interruption> _interruption = Interruption::None; ///< Written from any thread.
        std::optional<CallFailure> _fault;                            ///< Why the running call is failing.
        uint32_t _callDepth = 0; ///< Calls running, nested ones included.
    };

    std::expected<std::unique_ptr<IWasmInstance>, ExtensionError> WamrInstance::create(
        std::shared_ptr<IWasmModule const> owner,
        WASMModuleCommon* module,
        RuntimeLimits const& limits,
        std::span<ExportDescription const> exports,
        IHostContext& context)
    {
        auto const capPages = capPagesOf(limits);
        auto const exceedsCap = [&](ExportDescription const& entry) {
            return entry.kind == ExternKind::Memory && entry.initialPages > capPages;
        };
        if (std::ranges::any_of(exports, exceedsCap))
            return std::unexpected(ExtensionError::OutOfMemory);

        auto* rawArgs = static_cast<InstantiationArgs2*>(nullptr);
        if (!wasm_runtime_instantiation_args_create(&rawArgs))
            return std::unexpected(ExtensionError::RuntimeUnavailable);
        auto const args =
            std::unique_ptr<InstantiationArgs2, decltype(&wasm_runtime_instantiation_args_destroy)>(
                rawArgs, &wasm_runtime_instantiation_args_destroy);
        auto const stackBytes = saturated32(limits.stackBytes);
        wasm_runtime_instantiation_args_set_default_stack_size(args.get(), stackBytes);
        wasm_runtime_instantiation_args_set_host_managed_heap_size(args.get(), 0);
        wasm_runtime_instantiation_args_set_max_memory_pages(args.get(), capPages);

        auto errorBuffer = std::array<char, 128> {};
        auto instance = detail::InstanceHandle { wasm_runtime_instantiate_ex2(
            module, args.get(), errorBuffer.data(), static_cast<uint32_t>(errorBuffer.size())) };
        if (!instance)
            return std::unexpected(ExtensionError::InvalidComponent);

        // WAMR raises a cap below a memory's initial size to that size, and reads a cap of 0 as none: a
        // memory the exports did not show (or any memory, when not one page fits) is checked here.
        if (auto* const memory = wasm_runtime_get_default_memory(instance.get());
            memory && (capPages == 0 || wasm_memory_get_cur_page_count(memory) > capPages))
            return std::unexpected(ExtensionError::OutOfMemory);

        auto execEnv = detail::ExecEnvHandle { wasm_runtime_create_exec_env(instance.get(), stackBytes) };
        if (!execEnv)
            return std::unexpected(ExtensionError::RuntimeUnavailable);

        auto functions = std::vector<FunctionEntry> {};
        for (auto const& entry: exports | std::views::filter([](ExportDescription const& candidate) {
                                    return candidate.kind == ExternKind::Function;
                                }))
        {
            auto* const function = wasm_runtime_lookup_function(instance.get(), entry.name.c_str());
            if (!function)
                continue;
            functions.push_back(
                FunctionEntry { .name = entry.name,
                                .function = function,
                                .parameterCount = wasm_func_get_param_count(function, instance.get()),
                                .resultCount = wasm_func_get_result_count(function, instance.get()) });
        }

        return std::make_unique<WamrInstance>(PrivateTag {},
                                              std::move(owner),
                                              context,
                                              std::move(instance),
                                              std::move(execEnv),
                                              std::move(functions));
    }

} // namespace

std::expected<std::unique_ptr<IWasmInstance>, ExtensionError> instantiateModule(
    std::shared_ptr<IWasmModule const> owner,
    WASMModuleCommon* module,
    RuntimeLimits const& limits,
    std::span<ExportDescription const> exports,
    IHostContext& context)
{
    return WamrInstance::create(std::move(owner), module, limits, exports, context);
}

namespace
{
    /// @return The instance of this backend whose call is running in @p execEnv; null if there is none
    ///         (the execution environment is someone else's, or there is no call).
    [[nodiscard]] WamrInstance* instanceOf(wasm_exec_env_t execEnv) noexcept
    {
        if (!execEnv)
            return nullptr;
        return static_cast<WamrInstance*>(wasm_runtime_get_user_data(execEnv));
    }

    /// Runs the host function a trampoline was called for, with its flat arguments.
    void dispatchHostCall(wasm_exec_env_t execEnv, std::span<uint32_t const> arguments) noexcept
    {
        auto* const instance = instanceOf(execEnv);
        auto const* const index =
            static_cast<std::size_t const*>(wasm_runtime_get_function_attachment(execEnv));
        if (!instance || !index)
        {
            wasm_runtime_set_exception(wasm_runtime_get_module_inst(execEnv),
                                       "host function called outside an instance of its backend");
            return;
        }
        instance->callHost(*index, arguments);
    }

    /// The raw native WAMR calls a host function taking @p Arity i32s through: in WAMR's raw calling
    /// convention, each argument occupies one 64-bit slot. The slots are mutable, as WAMR declares them
    /// (they receive a result); host functions return none, so the trampolines only read them. The type
    /// must be WAMR's exactly: calling through a pointer of another function type is undefined.
    template <std::size_t Arity>
    void hostTrampoline(wasm_exec_env_t execEnv, uint64_t* rawArguments) noexcept
    {
        uint64_t* const first = rawArguments; // WAMR's slots, of WAMR's type: see above.
        auto const slots = std::span<uint64_t, Arity> { first, Arity };
        auto arguments = std::array<uint32_t, Arity> {};
        std::ranges::transform(
            slots, arguments.begin(), [](uint64_t slot) { return static_cast<uint32_t>(slot); });
        dispatchHostCall(execEnv, arguments);
    }

    /// A raw native, of the very type WAMR calls it through.
    using RawNative = void (*)(wasm_exec_env_t, uint64_t*);

    /// @return The trampolines for 0 to MaxHostParameters parameters, indexed by parameter count.
    template <std::size_t... Arities>
    [[nodiscard]] consteval auto makeTrampolines(std::index_sequence<Arities...> /*arities*/)
    {
        return std::array<RawNative, sizeof...(Arities)> { &hostTrampoline<Arities>... };
    }

    /// The trampolines, indexed by parameter count.
    constexpr auto Trampolines = makeTrampolines(std::make_index_sequence<MaxHostParameters + 1> {});

    /// The signature of a host function taking @p Arity i32s, in WAMR's notation: `(` an `i` per i32 `)`,
    /// NUL-terminated.
    template <std::size_t Arity>
    constexpr auto SignatureText = [] {
        auto text = std::array<char, Arity + 3> {};
        text.front() = '(';
        std::ranges::fill(std::span { text }.subspan(1, Arity), 'i');
        text[Arity + 1] = ')';
        return text;
    }();

    /// @return The signatures for 0 to MaxHostParameters parameters, indexed by parameter count.
    template <std::size_t... Arities>
    [[nodiscard]] consteval auto makeSignatures(std::index_sequence<Arities...> /*arities*/)
    {
        return std::array<char const*, sizeof...(Arities)> { SignatureText<Arities>.data()... };
    }

    /// The signatures in WAMR's notation, indexed by parameter count.
    constexpr auto Signatures = makeSignatures(std::make_index_sequence<MaxHostParameters + 1> {});
    static_assert(Signatures.size() == Trampolines.size(), "a signature for every trampoline");
    static_assert(std::string_view { SignatureText<3>.data() } == "(iii)");

    /// @return The process-lived attachments naming host function indices to the trampolines.
    [[nodiscard]] std::array<std::size_t, MaxHostFunctions>& attachments() noexcept
    {
        static auto indices = [] {
            auto values = std::array<std::size_t, MaxHostFunctions> {};
            std::ranges::copy(std::views::iota(std::size_t { 0 }, MaxHostFunctions), values.begin());
            return values;
        }();
        return indices;
    }

    /// The handler of WAMR's memory-growth callback.
    void onMemoryGrowthRefused(uint32_t /*pages*/,
                               uint64_t /*currentBytes*/,
                               uint32_t /*memoryIndex*/,
                               enlarge_memory_error_reason_t /*reason*/,
                               wasm_module_inst_t /*moduleInstance*/,
                               wasm_exec_env_t execEnv,
                               void* /*userData*/)
    {
        if (auto* const instance = instanceOf(execEnv))
            instance->refuseGrowth();
    }
} // namespace

std::optional<HostNative> hostNative(uint32_t parameterCount, std::size_t index) noexcept
{
    if (parameterCount > MaxHostParameters || index >= MaxHostFunctions)
        return std::nullopt;
    return HostNative { .function = reinterpret_cast<void*>(Trampolines.at(parameterCount)),
                        .signature = Signatures.at(parameterCount),
                        .attachment = &attachments().at(index) };
}

namespace detail
{
    void installMemoryGrowthHandler()
    {
        static auto once = std::once_flag {};
        std::call_once(once,
                       [] { wasm_runtime_set_enlarge_mem_error_callback(&onMemoryGrowthRefused, nullptr); });
    }
} // namespace detail

} // namespace vtextension::wasm::wamr
