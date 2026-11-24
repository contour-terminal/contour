// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/wasmtime/WasmtimeBackend.hpp>

#include <core/Utils.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <wasmtime.h>

#include <vtextension/wasm/EngineFeatures.hpp>
#include <vtextension/wasm/wasmtime/WasmtimeHandles.hpp>
#include <vtextension/wasm/wasmtime/WasmtimeRuntime.hpp>

// Every feature switch the table drives must exist: a Wasmtime built without one of these could
// neither be told to refuse what the table forbids nor be trusted to accept what it allows.
#if !defined(WASMTIME_FEATURE_COMPILER) || !defined(WASMTIME_FEATURE_GC)                     \
    || !defined(WASMTIME_FEATURE_THREADS) || !defined(WASMTIME_FEATURE_PARALLEL_COMPILATION) \
    || !defined(WASMTIME_FEATURE_COMPONENT_MODEL) || !defined(WASMTIME_FEATURE_COMPONENT_MODEL_ASYNC)
    #error "The extension host needs a Wasmtime built with its default features (wasmtime/conf.h)."
#endif

namespace vtextension::wasm::wasmtime
{

namespace
{
    /// Which kind and generation the calling thread is prepared for; generation 0 for none.
    struct ThreadStamp
    {
        KindState const* kind = nullptr; ///< The kind whose guard prepared the thread.
        uint64_t generation = 0;         ///< The generation it was prepared for.
    };

    thread_local auto threadStamp = ThreadStamp {};

    /// Whether a guard is the one that prepared its thread, or nests inside that one.
    enum class GuardRole : uint8_t
    {
        Nested,    ///< The thread was prepared already; ending the guard leaves it so.
        Outermost, ///< The guard prepared the thread; ending it ends the preparation.
    };

    /// A thread's preparation: a stamp of the generation it was entered for.
    class ThreadGuard final: public IThreadGuard
    {
      public:
        ThreadGuard(KindState const& kind, uint64_t generation, GuardRole role) noexcept:
            _kind { &kind }, _generation { generation }, _role { role }
        {
        }

        ThreadGuard(ThreadGuard const&) = delete;
        ThreadGuard(ThreadGuard&&) = delete;
        ThreadGuard& operator=(ThreadGuard const&) = delete;
        ThreadGuard& operator=(ThreadGuard&&) = delete;

        ~ThreadGuard() override
        {
            // A guard entered for an earlier generation leaves alone a later guard's stamp.
            if (_role == GuardRole::Outermost && threadStamp.kind == _kind
                && threadStamp.generation == _generation)
                threadStamp = ThreadStamp {};
        }

        [[nodiscard]] uint64_t generation() const noexcept override { return _generation; }

      private:
        KindState const* _kind;
        uint64_t _generation;
        GuardRole _role;
    };

    /// A Wasmtime switch for one feature. The bool is Wasmtime's C signature.
    using FeatureSetter = void (*)(wasm_config_t*, bool);

    /// One row of the switch table: a feature and Wasmtime's switch for it.
    struct FeatureSetterRow
    {
        EngineFeature feature {};       ///< The feature.
        FeatureSetter setter = nullptr; ///< Wasmtime's switch for it.
    };

    /// Wasmtime's switch for every feature of EngineFeatures that has one (FeatureSwitch::Configurable),
    /// in the table's order. Every engine is configured by setting each from its row's state.
    constexpr auto FeatureSetters = std::to_array<FeatureSetterRow>({
        { .feature = EngineFeature::BulkMemory, .setter = &wasmtime_config_wasm_bulk_memory_set },
        { .feature = EngineFeature::MultiValue, .setter = &wasmtime_config_wasm_multi_value_set },
        { .feature = EngineFeature::ReferenceTypes, .setter = &wasmtime_config_wasm_reference_types_set },
        { .feature = EngineFeature::Gc, .setter = &wasmtime_config_wasm_gc_set },
        { .feature = EngineFeature::Exceptions, .setter = &wasmtime_config_wasm_exceptions_set },
        { .feature = EngineFeature::Threads, .setter = &wasmtime_config_wasm_threads_set },
        { .feature = EngineFeature::Memory64, .setter = &wasmtime_config_wasm_memory64_set },
        { .feature = EngineFeature::MultiMemory, .setter = &wasmtime_config_wasm_multi_memory_set },
        { .feature = EngineFeature::TailCalls, .setter = &wasmtime_config_wasm_tail_call_set },
        { .feature = EngineFeature::Simd, .setter = &wasmtime_config_wasm_simd_set },
        { .feature = EngineFeature::RelaxedSimd, .setter = &wasmtime_config_wasm_relaxed_simd_set },
        { .feature = EngineFeature::ComponentModel, .setter = &wasmtime_config_wasm_component_model_set },
        { .feature = EngineFeature::FunctionReferences,
          .setter = &wasmtime_config_wasm_function_references_set },
        { .feature = EngineFeature::RelaxedSimdDeterministic,
          .setter = &wasmtime_config_wasm_relaxed_simd_deterministic_set },
        { .feature = EngineFeature::WideArithmetic, .setter = &wasmtime_config_wasm_wide_arithmetic_set },
        { .feature = EngineFeature::BranchHinting, .setter = &wasmtime_config_wasm_branch_hinting_set },
        { .feature = EngineFeature::CustomPageSizes, .setter = &wasmtime_config_wasm_custom_page_sizes_set },
        { .feature = EngineFeature::StackSwitching, .setter = &wasmtime_config_wasm_stack_switching_set },
        { .feature = EngineFeature::ComponentModelAsync,
          .setter = &wasmtime_config_wasm_component_model_async_set },
        { .feature = EngineFeature::ComponentModelAsyncBuiltins,
          .setter = &wasmtime_config_wasm_component_model_more_async_builtins_set },
        { .feature = EngineFeature::ComponentModelAsyncStackful,
          .setter = &wasmtime_config_wasm_component_model_async_stackful_set },
        { .feature = EngineFeature::ComponentModelMap,
          .setter = &wasmtime_config_wasm_component_model_map_set },
    });

    /// @return Whether FeatureSetters holds exactly the configurable features of EngineFeatures, each
    ///         once, in the table's order, each with a switch.
    [[nodiscard]] consteval bool featureSettersMatchTheTable() noexcept
    {
        auto configurable = EngineFeatures | std::views::filter([](FeatureRow const& row) {
                                return row.featureSwitch == FeatureSwitch::Configurable;
                            })
                            | std::views::transform(&FeatureRow::feature);
        return std::ranges::equal(configurable,
                                  FeatureSetters | std::views::transform(&FeatureSetterRow::feature))
               && std::ranges::none_of(FeatureSetters,
                                       [](FeatureSetterRow const& row) { return row.setter == nullptr; });
    }
    static_assert(featureSettersMatchTheTable(),
                  "FeatureSetters drives every switchable row of EngineFeatures");

    /// @return A configuration for one runtime's engine, from EngineFeatures, @p config and @p limits.
    [[nodiscard]] wasm_config_t* engineConfig(WasmtimeConfig config, RuntimeLimits limits)
    {
        auto* const engineConfig = wasm_config_new();
        for (auto const& [feature, setter]: FeatureSetters)
            setter(engineConfig, featureRow(feature).state == FeatureState::On);
        wasmtime_config_max_wasm_stack_set(engineConfig, limits.stackBytes);
        // Epoch interruption is how a call is stopped from another thread. A store's epoch deadline
        // starts at 0 ticks, so a guest traps at its first epoch check: whoever enters guest code sets
        // the store's deadline (wasmtime_context_set_epoch_deadline) before every call into it.
        wasmtime_config_epoch_interruption_set(engineConfig, true);
        wasmtime_config_signals_based_traps_set(engineConfig, config.trapMode == TrapMode::Signals);
        wasmtime_config_cranelift_opt_level_set(engineConfig, WASMTIME_OPT_LEVEL_SPEED);
        // Compilation runs on the loading thread, rather than on threads of Wasmtime's own.
        wasmtime_config_parallel_compilation_set(engineConfig, false);
        // Mach ports would install a process-wide exception handler thread on macOS.
        wasmtime_config_macos_use_mach_ports_set(engineConfig, false);
        return engineConfig;
    }

    /// Runs a host function on behalf of the guest: hands its i32 arguments to the IHostContext that is
    /// the data of the calling instance's store.
    /// Called from Wasmtime's Rust code, which an exception must not unwind through: noexcept.
    /// @return Null on success; a trap if there is no host context or it failed.
    wasm_trap_t* callHostFunction(void* env,
                                  wasmtime_caller_t* caller,
                                  wasmtime_val_t const* args,
                                  std::size_t argCount,
                                  wasmtime_val_t* /*results*/,
                                  std::size_t /*resultCount*/) noexcept
    {
        auto const& binding = *static_cast<HostBinding const*>(env);
        auto* const context =
            static_cast<IHostContext*>(wasmtime_context_get_data(wasmtime_caller_context(caller)));
        if (!context)
        {
            static constexpr auto Message = std::string_view { "no host context" };
            return wasmtime_trap_new(Message.data(), Message.size());
        }
        auto const arguments = std::ranges::to<std::vector>(
            std::span { args, argCount } | std::views::transform([](wasmtime_val_t const& value) {
                return static_cast<uint32_t>(value.of.i32);
            }));
        if (!context->callHost(binding.index, arguments))
        {
            static constexpr auto Message = std::string_view { "host function failed" };
            return wasmtime_trap_new(Message.data(), Message.size());
        }
        return nullptr;
    }

    /// @return The type of a host function taking @p parameterCount i32s and returning nothing.
    [[nodiscard]] detail::FuncTypeHandle hostFunctionType(uint32_t parameterCount)
    {
        auto parameters = wasm_valtype_vec_t {};
        wasm_valtype_vec_new_uninitialized(&parameters, parameterCount);
        for (auto& parameter: std::span { parameters.data, parameters.size })
            parameter = wasm_valtype_new(WASM_I32);
        auto results = wasm_valtype_vec_t {};
        wasm_valtype_vec_new_empty(&results);
        return detail::FuncTypeHandle { wasm_functype_new(&parameters, &results) };
    }

    /// @return A linker on @p engine providing every host function of @p hostBindings; nothing if one
    ///         cannot be defined.
    [[nodiscard]] std::optional<detail::LinkerHandle> makeLinker(wasm_engine_t* engine,
                                                                 std::span<HostBinding> hostBindings)
    {
        auto linker = detail::LinkerHandle { wasmtime_linker_new(engine) };
        if (!linker)
            return std::nullopt;
        for (auto& binding: hostBindings)
        {
            auto const type = hostFunctionType(binding.parameterCount);
            auto const error = detail::ErrorHandle { wasmtime_linker_define_func(linker.get(),
                                                                                 binding.module.data(),
                                                                                 binding.module.size(),
                                                                                 binding.name.data(),
                                                                                 binding.name.size(),
                                                                                 type.get(),
                                                                                 &callHostFunction,
                                                                                 &binding,
                                                                                 nullptr) };
            if (error)
                return std::nullopt;
        }
        return linker;
    }

    /// A Wasmtime backend: a facade over the kind's shared state.
    class WasmtimeBackend final: public IWasmBackend
    {
      public:
        WasmtimeBackend(std::shared_ptr<KindState> kind, WasmtimeConfig config) noexcept:
            _kind { std::move(kind) }, _config { config }
        {
        }

        [[nodiscard]] std::expected<std::shared_ptr<IWasmRuntime>, ExtensionError> createRuntime(
            RuntimeLimits limits, std::span<HostFunction const> hostFunctions) override
        {
            return WasmtimeRuntime::create(_kind, _config, limits, hostFunctions)
                .transform([](std::shared_ptr<WasmtimeRuntime> runtime) -> std::shared_ptr<IWasmRuntime> {
                    return runtime;
                });
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
            return BackendCapabilities { .memoryGrowth = MemoryGrowthReporting::Unreported };
        }

      private:
        std::shared_ptr<KindState> _kind;
        WasmtimeConfig _config;
    };
} // namespace

void KindState::acquireRuntime() noexcept
{
    auto const lock = std::scoped_lock { _mutex };
    if (_liveRuntimes++ == 0)
        ++_generation;
}

void KindState::releaseRuntime() noexcept
{
    auto const lock = std::scoped_lock { _mutex };
    --_liveRuntimes;
}

std::expected<std::unique_ptr<IThreadGuard>, ExtensionError> KindState::enterThread()
{
    auto const lock = std::scoped_lock { _mutex };
    if (_liveRuntimes == 0)
        return std::unexpected(ExtensionError::RuntimeUnavailable);
    auto const current = _generation.load();
    if (threadStamp.kind == this && threadStamp.generation == current)
        return std::make_unique<ThreadGuard>(*this, current, GuardRole::Nested);
    threadStamp = ThreadStamp { .kind = this, .generation = current };
    return std::make_unique<ThreadGuard>(*this, current, GuardRole::Outermost);
}

bool KindState::isCurrentThreadPrepared() const noexcept
{
    return threadStamp.kind == this && threadStamp.generation != 0
           && threadStamp.generation == _generation.load();
}

uint64_t KindState::preparedGeneration() noexcept
{
    return threadStamp.generation;
}

std::expected<std::shared_ptr<WasmtimeRuntime>, ExtensionError> WasmtimeRuntime::create(
    std::shared_ptr<KindState> kind,
    WasmtimeConfig config,
    RuntimeLimits limits,
    std::span<HostFunction const> hostFunctions)
{
    // wasm_engine_new_with_config takes the configuration over.
    auto engine = detail::EngineHandle { wasm_engine_new_with_config(engineConfig(config, limits)) };
    if (!engine)
        return std::unexpected(ExtensionError::RuntimeUnavailable);
    auto hostBindings = std::vector<HostBinding> {};
    hostBindings.reserve(hostFunctions.size());
    for (auto const [index, function]: core::views::enumerate(hostFunctions))
        hostBindings.push_back(HostBinding { .module = std::string { function.module },
                                             .name = std::string { function.name },
                                             .index = static_cast<std::size_t>(index),
                                             .parameterCount = function.parameterCount });
    // The linker's callbacks point into hostBindings' buffer, which moving the vector keeps.
    auto linker = makeLinker(engine.get(), hostBindings);
    if (!linker)
        return std::unexpected(ExtensionError::RuntimeUnavailable);
    return std::make_shared<WasmtimeRuntime>(
        std::move(kind), limits, std::move(engine), std::move(hostBindings), std::move(*linker));
}

WasmtimeRuntime::WasmtimeRuntime(std::shared_ptr<KindState> kind,
                                 RuntimeLimits limits,
                                 detail::EngineHandle engine,
                                 std::vector<HostBinding> hostBindings,
                                 detail::LinkerHandle linker) noexcept:
    _kind { std::move(kind) },
    _limits { limits },
    _engine { std::move(engine) },
    _hostBindings { std::move(hostBindings) },
    _linker { std::move(linker) }
{
    _kind->acquireRuntime();
}

WasmtimeRuntime::~WasmtimeRuntime()
{
    // The engine goes before the runtime stops counting alive, so that no bring-up overlaps it.
    _linker.reset();
    _engine.reset();
    _kind->releaseRuntime();
}

std::expected<std::shared_ptr<IWasmModule>, ExtensionError> WasmtimeRuntime::load(
    std::vector<std::byte> bytes)
{
    return loadModule(shared_from_this(), _engine.get(), bytes);
}

std::shared_ptr<KindState> makeKindState()
{
    return std::make_shared<KindState>();
}

std::unique_ptr<IWasmBackend> makeWasmtimeBackend(std::shared_ptr<KindState> kind, WasmtimeConfig config)
{
    return std::make_unique<WasmtimeBackend>(std::move(kind), config);
}

} // namespace vtextension::wasm::wasmtime
