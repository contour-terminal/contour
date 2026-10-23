// SPDX-License-Identifier: Apache-2.0
#include <vtextension/Instance.hpp>

#include <core/Assert.hpp>
#include <core/platform/Clock.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <wasm_export.h>

#include <vtextension/CanonicalAbi.hpp>
#include <vtextension/HostBindings.hpp>
#include <vtextension/StatuslineWorld.hpp>
#include <vtextension/wasm/wamr/WamrRuntime.hpp>

namespace vtextension
{

namespace
{
    /// The bytes of one WebAssembly page.
    constexpr auto WasmPageBytes = std::size_t { 64 } * 1024;

    /// What the runtime's exception says when the watchdog terminated the instance.
    constexpr auto TerminatedException = std::string_view { "terminated by user" };

    /// The Canonical ABI's limit on flat parameters; a call with more passes them through memory.
    constexpr auto MaxFlatParameters = std::size_t { 16 };

    static_assert(abi::Codec<Event>::FlatCount <= MaxFlatParameters, "update takes its event flat");
    static_assert(abi::Codec<ConfigMap>::FlatCount <= MaxFlatParameters, "load takes its config flat");

    /// @return The Instance a runtime call is running in; null if the instance is not one of ours.
    [[nodiscard]] Instance* instanceOf(wasm_module_inst_t moduleInstance) noexcept
    {
        return static_cast<Instance*>(wasm_runtime_get_custom_data(moduleInstance));
    }

    /// @return @p extent as a cell count of the world's `u32`; negative extents count as zero.
    [[nodiscard]] uint32_t cellCount(int extent) noexcept
    {
        return static_cast<uint32_t>(std::max(extent, 0));
    }
} // namespace

/// The guest's linear memory as the codec reaches it: every view is bounds-checked by the runtime,
/// allocations go through the guest's `cabi_realloc` and are checked as well, and what lifting may
/// spend on the host is bounded per host call.
class Instance::GuestMemory final: public abi::IGuestMemory
{
  public:
    explicit GuestMemory(Instance& instance) noexcept: _instance { instance } {}

    /// Gives the next lift the full budget of one host call.
    void resetLiftBudget() noexcept { _liftBudget = _instance._limits.maxHostCallBytes; }

    [[nodiscard]] abi::Result<std::span<std::byte>> view(abi::Address address, uint32_t size) override
    {
        if (size == 0)
            return std::span<std::byte> {};
        auto* const moduleInstance = _instance._instance.get();
        if (!wasm_runtime_validate_app_addr(moduleInstance, address, size))
            return std::unexpected(ExtensionError::Trapped);
        auto* const native =
            static_cast<std::byte*>(wasm_runtime_addr_app_to_native(moduleInstance, address));
        return std::span<std::byte> { native, size };
    }

    [[nodiscard]] abi::Result<abi::Address> allocate(uint32_t size, uint32_t alignment) override
    {
        auto argv = std::array<uint32_t, 4> { 0, 0, alignment, size }; // old pointer, old size, align, size
        if (auto called = _instance.callExport(_instance._exports.realloc, argv); !called)
            return std::unexpected(called.error());
        // The guest's allocator is the guest's: what it answers is checked like any other address.
        auto const address = argv[0];
        if (alignment == 0 || address % alignment != 0)
            return std::unexpected(ExtensionError::Trapped);
        return view(address, size).transform([&](std::span<std::byte>) { return address; });
    }

    [[nodiscard]] abi::Result<void> chargeLift(uint64_t bytes) override
    {
        if (bytes > _liftBudget)
            return std::unexpected(ExtensionError::Trapped);
        _liftBudget -= bytes;
        return {};
    }

  private:
    Instance& _instance;
    uint64_t _liftBudget = 0; ///< What lifting may still spend in the current host call.
};

/// The world's host functions, as the runtime calls them: each receives the core-wasm parameters
/// the Canonical ABI flattens its WIT signature to, lifts them, calls the IHostServices, and lowers
/// the answer. A guest that breaks the ABI (an address outside its memory, a string that is not
/// UTF-8, an unknown enum case) traps.
struct Instance::HostCalls
{
    /// Runs @p body for the Instance the call came from, or traps the guest if there is none.
    ///
    /// This is called by the interpreter, which is C: no exception may leave it. One that @p body
    /// throws (the host could not allocate, a host service failed) faults the call instead, and so
    /// does a host call made while another is in progress.
    template <typename Body>
    static void dispatch(wasm_exec_env_t execEnv, Body const& body) noexcept
    {
        auto* const moduleInstance = wasm_runtime_get_module_inst(execEnv);
        auto* const instance = instanceOf(moduleInstance);
        if (!instance)
        {
            wasm_runtime_set_exception(moduleInstance, "host function called outside an extension instance");
            return;
        }
        // Host calls do not nest. The only way into one from inside another is the guest's
        // `cabi_realloc`, which the host calls while it stores a result; a guest that makes a host
        // call from there would run host code against a half-stored result.
        if (instance->_hostCall == HostCallState::InProgress)
        {
            instance->failHostCall(ExtensionError::Trapped);
            return;
        }
        instance->_hostCall = HostCallState::InProgress;
        auto result = abi::Result<void> {};
        try
        {
            instance->_memory->resetLiftBudget();
            result = body(*instance);
        }
        catch (...)
        {
            result = std::unexpected(ExtensionError::Trapped);
        }
        instance->_hostCall = HostCallState::Idle;
        if (!result)
            instance->failHostCall(result.error());
    }

    /// `request-frame: func(after-ms: u32)`.
    static void requestFrame(wasm_exec_env_t execEnv, uint32_t afterMs)
    {
        dispatch(execEnv, [&](Instance& instance) -> abi::Result<void> {
            instance._services.requestFrame(std::chrono::milliseconds { afterMs });
            return {};
        });
    }

    /// `log: func(level: log-level, message: string)`.
    static void log(wasm_exec_env_t execEnv, uint32_t level, uint32_t pointer, uint32_t length)
    {
        dispatch(execEnv, [&](Instance& instance) -> abi::Result<void> {
            auto const flat = std::array { level, pointer, length };
            auto reader = abi::FlatReader(flat);
            auto const lifted = abi::Codec<LogLevel>::liftFlat(*instance._memory, reader);
            if (!lifted)
                return std::unexpected(lifted.error());
            return abi::Codec<std::string>::liftFlat(*instance._memory, reader)
                .transform([&](std::string const& message) { instance._services.log(*lifted, message); });
        });
    }

    /// `state: func() -> state-snapshot`, whose result the guest receives at @p resultAddress.
    static void state(wasm_exec_env_t execEnv, uint32_t resultAddress)
    {
        dispatch(execEnv, [&](Instance& instance) {
            return abi::Codec<StateSnapshot>::store(
                *instance._memory, resultAddress, instance._services.state());
        });
    }

    /// `run-command: func(name: string, args: list<tuple<string, string>>) ->
    /// result<command-output, command-error>`, whose result the guest receives at @p resultAddress.
    static void runCommand(wasm_exec_env_t execEnv,
                           uint32_t namePointer,
                           uint32_t nameLength,
                           uint32_t argsPointer,
                           uint32_t argsLength,
                           uint32_t resultAddress)
    {
        dispatch(execEnv, [&](Instance& instance) -> abi::Result<void> {
            auto& memory = *instance._memory;
            auto const flat = std::array { namePointer, nameLength, argsPointer, argsLength };
            auto reader = abi::FlatReader(flat);
            auto const name = abi::Codec<std::string>::liftFlat(memory, reader);
            if (!name)
                return std::unexpected(name.error());
            auto const args = abi::Codec<ConfigMap>::liftFlat(memory, reader);
            if (!args)
                return std::unexpected(args.error());
            auto const result = run(instance, *name, *args);
            if (!result)
                return std::unexpected(result.error());
            return abi::Codec<abi::CommandResult>::store(memory, resultAddress, *result);
        });
    }

    /// Runs the command a `run-command` call names, with the guest's deadline standing still: the
    /// command's run time is the host's to bound, and its limit is far longer than a call's
    /// deadline.
    /// @return The answer for the guest; `DeadlineExceeded` if the call's deadline was spent before
    ///         the command could start, in which case it is not run.
    [[nodiscard]] static abi::Result<abi::CommandResult> run(Instance& instance,
                                                             std::string const& name,
                                                             ConfigMap const& args)
    {
        auto const token = WatchToken { .instance = instance._instance.get() };
        auto const remaining = instance._deadline - instance._clock.now();
        instance._engine->disarm(token);
        // Once disarmed, the watchdog cannot fire any more; it may have already, or the deadline may
        // be spent all the same.
        if (wasm_runtime_get_exception(instance._instance.get())
            || remaining <= core::platform::SteadyDuration::zero())
            return std::unexpected(ExtensionError::DeadlineExceeded);

        auto output = instance._services.runCommand(name, args);

        instance._deadline = instance._clock.now() + remaining;
        instance._engine->armDeadline(token, std::chrono::ceil<std::chrono::milliseconds>(remaining));

        return std::move(output)
            .transform([](std::string const& stdoutText) {
                // The host reports a finished command by its output; a command that did not exit
                // normally is an error, so one that finished counts as exit code 0.
                return abi::CommandOutput { .exitCode = 0,
                                            .standardOutput = ByteList(stdoutText.begin(), stdoutText.end()),
                                            .standardError = {} };
            })
            .transform_error([](ExtensionError error) { return abi::commandErrorFor(error); });
    }

    /// The runtime's callback for a memory that could not grow.
    static void onMemoryGrowthRefused(uint32_t /*pages*/,
                                      uint64_t /*currentBytes*/,
                                      uint32_t /*memoryIndex*/,
                                      enlarge_memory_error_reason_t /*reason*/,
                                      wasm_module_inst_t moduleInstance,
                                      wasm_exec_env_t /*execEnv*/,
                                      void* /*userData*/)
    {
        if (auto* const instance = instanceOf(moduleInstance))
            instance->_memoryPressure = MemoryPressure::GrowthRefused;
    }
};

namespace detail
{
    namespace
    {
        /// One host function, as the runtime registers it.
        struct HostFunction
        {
            char const* name;      ///< The import's function name.
            void* function;        ///< Its implementation.
            char const* signature; ///< Its core signature, in the runtime's notation: `i` is an i32.
        };

        /// A registration of the host functions. The runtime keeps (and sorts) the very array it is
        /// given, so each registration owns its own until it unregisters it.
        class NativeRegistration final: public HostRegistration
        {
          public:
            explicit NativeRegistration(std::array<HostFunction, 4> const& functions)
            {
                std::ranges::transform(functions, _symbols.begin(), [](HostFunction const& function) {
                    return NativeSymbol { .symbol = function.name,
                                          .func_ptr = function.function,
                                          .signature = function.signature,
                                          .attachment = nullptr };
                });
            }

            NativeRegistration(NativeRegistration const&) = delete;
            NativeRegistration(NativeRegistration&&) = delete;
            NativeRegistration& operator=(NativeRegistration const&) = delete;
            NativeRegistration& operator=(NativeRegistration&&) = delete;

            ~NativeRegistration() override
            {
                if (_state == State::Registered)
                    wasm_runtime_unregister_natives(HostImportModule, _symbols.data());
            }

            /// Registers the array with the runtime, which may refuse it.
            void registerSymbols()
            {
                if (wasm_runtime_register_natives(
                        HostImportModule, _symbols.data(), static_cast<uint32_t>(_symbols.size())))
                    _state = State::Registered;
            }

            /// @return Whether the runtime holds the array.
            [[nodiscard]] bool isRegistered() const noexcept { return _state == State::Registered; }

          private:
            /// Whether the runtime holds _symbols, which it must then be asked to give back.
            enum class State : uint8_t
            {
                Unregistered,
                Registered,
            };

            std::array<NativeSymbol, 4> _symbols {};
            State _state = State::Unregistered;
        };
    } // namespace

    std::unique_ptr<HostRegistration> registerHostFunctions()
    {
        // The callback is the runtime's one global, which the WAMR backend owns: it hands the failures
        // of module instances it did not create (these) on to this handler.
        wasm::wamr::detail::forwardForeignMemoryGrowth(&Instance::HostCalls::onMemoryGrowthRefused);
        // The world's host functions.
        auto const functions = std::array {
            HostFunction { .name = "request-frame",
                           .function = reinterpret_cast<void*>(&Instance::HostCalls::requestFrame),
                           .signature = "(i)" },
            HostFunction { .name = "log",
                           .function = reinterpret_cast<void*>(&Instance::HostCalls::log),
                           .signature = "(iii)" },
            HostFunction { .name = "state",
                           .function = reinterpret_cast<void*>(&Instance::HostCalls::state),
                           .signature = "(i)" },
            HostFunction { .name = "run-command",
                           .function = reinterpret_cast<void*>(&Instance::HostCalls::runCommand),
                           .signature = "(iiiii)" },
        };
        auto registration = std::make_unique<NativeRegistration>(functions);
        registration->registerSymbols();
        if (!registration->isRegistered())
            return nullptr;
        return registration;
    }
} // namespace detail

Instance::Instance(PrivateTag /*unused*/,
                   std::shared_ptr<CompiledExtension const> compiled,
                   IHostServices& services,
                   core::platform::IClock& clock,
                   ExtensionLimits limits):
    _compiled { std::move(compiled) },
    _engine { _compiled->engine().shared_from_this() },
    _services { services },
    _clock { clock },
    _limits { limits },
    _memory { std::make_unique<GuestMemory>(*this) }
{
}

Instance::~Instance() = default;

std::expected<std::unique_ptr<Instance>, ExtensionError> Instance::create(Engine& engine,
                                                                          CompiledExtension const& compiled,
                                                                          IHostServices& services,
                                                                          core::platform::IClock& clock,
                                                                          ExtensionLimits limits)
{
    Require(&engine == &compiled.engine());
    auto instance =
        std::make_unique<Instance>(PrivateTag {}, compiled.shared_from_this(), services, clock, limits);
    return instance->instantiate().transform([&] { return std::move(instance); });
}

std::expected<void, ExtensionError> Instance::instantiate()
{
    auto* const module = _compiled->module();

    auto memoryExport = wasm_export_t {};
    for (auto const index:
         std::views::iota(int32_t { 0 }, std::max(wasm_runtime_get_export_count(module), 0)))
    {
        wasm_runtime_get_export_type(module, index, &memoryExport);
        if (memoryExport.kind == WASM_IMPORT_EXPORT_KIND_MEMORY)
            break;
    }
    auto const capPages = _limits.memoryBytes / WasmPageBytes;
    if (wasm_memory_type_get_init_page_count(memoryExport.u.memory_type) > capPages)
        return std::unexpected(ExtensionError::OutOfMemory);

    auto* rawArgs = static_cast<InstantiationArgs2*>(nullptr);
    if (!wasm_runtime_instantiation_args_create(&rawArgs))
        return std::unexpected(ExtensionError::RuntimeUnavailable);
    auto const args = std::unique_ptr<InstantiationArgs2, decltype(&wasm_runtime_instantiation_args_destroy)>(
        rawArgs, &wasm_runtime_instantiation_args_destroy);
    auto const stackBytes = static_cast<uint32_t>(
        std::min<std::size_t>(_limits.stackBytes, std::numeric_limits<uint32_t>::max()));
    wasm_runtime_instantiation_args_set_default_stack_size(args.get(), stackBytes);
    wasm_runtime_instantiation_args_set_host_managed_heap_size(args.get(), 0);
    wasm_runtime_instantiation_args_set_max_memory_pages(args.get(), static_cast<uint32_t>(capPages));

    auto errorBuffer = std::array<char, 128> {};
    _instance.reset(wasm_runtime_instantiate_ex2(
        module, args.get(), errorBuffer.data(), static_cast<uint32_t>(errorBuffer.size())));
    if (!_instance)
        return std::unexpected(ExtensionError::InvalidComponent);
    wasm_runtime_set_custom_data(_instance.get(), this);

    _execEnv.reset(wasm_runtime_create_exec_env(_instance.get(), stackBytes));
    if (!_execEnv)
        return std::unexpected(ExtensionError::RuntimeUnavailable);

    auto const lookup = [&](char const* name) {
        return wasm_runtime_lookup_function(_instance.get(), name);
    };
    _exports = Exports { .load = lookup("load"),
                         .update = lookup("update"),
                         .render = lookup("render"),
                         .realloc = lookup("cabi_realloc"),
                         .postLoad = lookup("cabi_post_load"),
                         .postUpdate = lookup("cabi_post_update"),
                         .postRender = lookup("cabi_post_render"),
                         .initialize = lookup("_initialize") };
    if (!_exports.initialize)
        return {};
    return guarded(_limits.loadDeadline, [&] { return callExport(_exports.initialize, {}); });
}

template <typename Body>
auto Instance::guarded(std::chrono::milliseconds timeout, Body const& body) -> decltype(body())
{
    Require(Engine::ThreadEnv::isCurrentThreadPrepared());
    if (_fault)
        return std::unexpected(*_fault);

    _hostCallError.reset();
    _memory->resetLiftBudget();
    _memoryPressure = MemoryPressure::None;
    _deadline = _clock.now() + timeout;
    // Disarmed on every way out of the body, an exception included, which becomes a fault: no
    // deadline may outlive the call, or the watchdog could later terminate a destroyed instance.
    auto result = runGuarded(*_engine, WatchToken { .instance = _instance.get() }, timeout, body);

    if (!result)
        _fault = result.error();
    else if (wasm_runtime_get_exception(_instance.get()))
        // The deadline expired after the guest returned, while the host was still lifting the result:
        // the call overran all the same, and the instance is terminated.
        _fault = ExtensionError::DeadlineExceeded;

    if (_fault)
        return std::unexpected(*_fault);
    return result;
}

std::expected<void, ExtensionError> Instance::callExport(Function function, std::span<uint32_t> argv)
{
    if (wasm_runtime_call_wasm(_execEnv.get(), function, static_cast<uint32_t>(argv.size()), argv.data()))
        return {};
    return std::unexpected(guestFailure());
}

std::expected<void, ExtensionError> Instance::callPostReturn(Function function, uint32_t result)
{
    if (!function)
        return {};
    auto argv = std::array { result };
    return callExport(function, argv);
}

ExtensionError Instance::guestFailure() const
{
    if (_hostCallError)
        return *_hostCallError;
    auto const* const exception = wasm_runtime_get_exception(_instance.get());
    if (exception && std::string_view { exception }.contains(TerminatedException))
        return ExtensionError::DeadlineExceeded;
    if (_memoryPressure == MemoryPressure::GrowthRefused)
        return ExtensionError::OutOfMemory;
    return ExtensionError::Trapped;
}

void Instance::failHostCall(ExtensionError error) noexcept
{
    if (!_hostCallError)
        _hostCallError = error;
    // Called on the interpreter's side of a host call, so nothing may escape; the runtime copies
    // the message.
    try
    {
        wasm_runtime_set_exception(_instance.get(), std::string(errorText(error)).c_str());
    }
    catch (...)
    {
        wasm_runtime_set_exception(_instance.get(), "extension host call failed");
    }
}

std::expected<EventKinds, ExtensionError> Instance::load(ConfigMap const& config)
{
    return guarded(_limits.loadDeadline, [&]() -> std::expected<EventKinds, ExtensionError> {
        auto argv = abi::FlatValues {};
        if (auto lowered = abi::Codec<ConfigMap>::lowerFlat(*_memory, config, argv); !lowered)
            return std::unexpected(lowered.error());
        if (auto called = callExport(_exports.load, argv); !called)
            return std::unexpected(called.error());
        auto reader = abi::FlatReader(std::span { argv }.first(1));
        auto const kinds = abi::Codec<EventKinds>::liftFlat(*_memory, reader);
        if (!kinds)
            return std::unexpected(kinds.error());
        return callPostReturn(_exports.postLoad, argv.front()).transform([&] {
            return (*kinds & SubscribableKinds) | AlwaysDelivered;
        });
    });
}

std::expected<RenderRequest, ExtensionError> Instance::update(Event const& event)
{
    return guarded(_limits.callDeadline, [&]() -> std::expected<RenderRequest, ExtensionError> {
        auto argv = abi::FlatValues {};
        if (auto lowered = abi::Codec<Event>::lowerFlat(*_memory, event, argv); !lowered)
            return std::unexpected(lowered.error());
        if (auto called = callExport(_exports.update, argv); !called)
            return std::unexpected(called.error());
        auto reader = abi::FlatReader(std::span { argv }.first(1));
        auto const request = abi::Codec<RenderRequest>::liftFlat(*_memory, reader);
        if (!request)
            return std::unexpected(request.error());
        return callPostReturn(_exports.postUpdate, argv.front()).transform([&] { return *request; });
    });
}

std::expected<std::string, ExtensionError> Instance::render(vtpty::PageSize size)
{
    return guarded(_limits.callDeadline, [&]() -> std::expected<std::string, ExtensionError> {
        auto argv = std::array { cellCount(unbox(size.columns)), cellCount(unbox(size.lines)) };
        if (auto called = callExport(_exports.render, argv); !called)
            return std::unexpected(called.error());
        auto const resultAddress = argv.front();
        auto bytes = abi::loadByteString(*_memory, resultAddress, _limits.maxRenderBytes);
        if (!bytes)
            return std::unexpected(bytes.error());
        return callPostReturn(_exports.postRender, resultAddress).transform([&] {
            return std::move(*bytes);
        });
    });
}

} // namespace vtextension
