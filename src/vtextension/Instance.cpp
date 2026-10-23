// SPDX-License-Identifier: Apache-2.0
#include <vtextension/Instance.hpp>

#include <core/Assert.hpp>
#include <core/platform/Clock.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <vtextension/CanonicalAbi.hpp>
#include <vtextension/HostBindings.hpp>
#include <vtextension/StatuslineWorld.hpp>

namespace vtextension
{

namespace
{
    /// The bytes of one WebAssembly page.
    constexpr auto WasmPageBytes = uint64_t { 64 } * 1024;

    /// The Canonical ABI's limit on flat parameters; a call with more passes them through memory.
    constexpr auto MaxFlatParameters = std::size_t { 16 };

    static_assert(abi::Codec<Event>::FlatCount <= MaxFlatParameters, "update takes its event flat");
    static_assert(abi::Codec<ConfigMap>::FlatCount <= MaxFlatParameters, "load takes its config flat");

    /// @return @p extent as a cell count of the world's `u32`; negative extents count as zero.
    [[nodiscard]] uint32_t cellCount(int extent) noexcept
    {
        return static_cast<uint32_t>(std::max(extent, 0));
    }

    /// The world's host functions, as every runtime is created with them: the index of each row is
    /// what the runtime calls it by, and the row of Instance's handler table that runs it.
    constexpr auto HostFunctions = std::array {
        wasm::HostFunction {
            .module = detail::HostImportModule, .name = "request-frame", .parameterCount = 1 },
        wasm::HostFunction { .module = detail::HostImportModule, .name = "log", .parameterCount = 3 },
        wasm::HostFunction { .module = detail::HostImportModule, .name = "state", .parameterCount = 1 },
        wasm::HostFunction { .module = detail::HostImportModule, .name = "run-command", .parameterCount = 5 },
    };
} // namespace

namespace detail
{
    std::span<wasm::HostFunction const> statuslineHostFunctions() noexcept
    {
        return HostFunctions;
    }
} // namespace detail

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
        auto const bytes = _instance._instance->memory(address, size);
        if (!bytes)
            return std::unexpected(ExtensionError::Trapped);
        return *bytes;
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

/// The world's host functions: each receives the core-wasm parameters the Canonical ABI flattens its
/// WIT signature to, lifts them, calls the IHostServices, and lowers the answer. A guest that breaks
/// the ABI (an address outside its memory, a string that is not UTF-8, an unknown enum case) traps.
struct Instance::HostCalls
{
    /// What runs one host function, given its flat arguments (as many as its row in HostFunctions says).
    using Handler = abi::Result<void> (*)(Instance& instance, std::span<uint32_t const> arguments);

    /// `request-frame: func(after-ms: u32)`.
    static abi::Result<void> requestFrame(Instance& instance, std::span<uint32_t const> arguments)
    {
        instance._services.requestFrame(std::chrono::milliseconds { arguments[0] });
        return {};
    }

    /// `log: func(level: log-level, message: string)`.
    static abi::Result<void> log(Instance& instance, std::span<uint32_t const> arguments)
    {
        auto reader = abi::FlatReader(arguments);
        auto const lifted = abi::Codec<LogLevel>::liftFlat(*instance._memory, reader);
        if (!lifted)
            return std::unexpected(lifted.error());
        return abi::Codec<std::string>::liftFlat(*instance._memory, reader)
            .transform([&](std::string const& message) { instance._services.log(*lifted, message); });
    }

    /// `state: func() -> state-snapshot`, whose result the guest receives at the address it passes.
    static abi::Result<void> state(Instance& instance, std::span<uint32_t const> arguments)
    {
        return abi::Codec<StateSnapshot>::store(*instance._memory, arguments[0], instance._services.state());
    }

    /// `run-command: func(name: string, args: list<tuple<string, string>>) ->
    /// result<command-output, command-error>`, whose result the guest receives at the address it passes
    /// last.
    static abi::Result<void> runCommand(Instance& instance, std::span<uint32_t const> arguments)
    {
        auto& memory = *instance._memory;
        auto reader = abi::FlatReader(arguments.first(4));
        auto const resultAddress = arguments[4];
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
        auto const token = instance.watchToken();
        auto const remaining = instance._deadline - instance._clock.now();
        instance._engine->disarm(token);
        // Once disarmed, the watchdog cannot fire any more; it may have already, or the deadline may
        // be spent all the same.
        if (instance.isDeadlineExpired() || remaining <= core::platform::SteadyDuration::zero())
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

    /// What runs each host function, by its index in HostFunctions.
    static constexpr auto Handlers = std::array<Handler, 4> { &requestFrame, &log, &state, &runCommand };
    static_assert(Handlers.size() == HostFunctions.size(), "a handler for every host function");
};

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
    // The memory and stack caps are the engine's runtime's: an instance cannot be held to others.
    Require(limits.memoryBytes == engine.limits().memoryBytes
            && limits.stackBytes == engine.limits().stackBytes);
    auto instance =
        std::make_unique<Instance>(PrivateTag {}, compiled.shared_from_this(), services, clock, limits);
    return instance->instantiate().transform([&] { return std::move(instance); });
}

std::expected<void, ExtensionError> Instance::instantiate()
{
    auto& module = _compiled->module();

    auto const exceedsCap = [&](wasm::ExportDescription const& entry) {
        return entry.kind == wasm::ExternKind::Memory
               && uint64_t { entry.initialPages } * WasmPageBytes > _limits.memoryBytes;
    };
    if (std::ranges::any_of(module.exports(), exceedsCap))
        return std::unexpected(ExtensionError::OutOfMemory);

    auto instance = module.instantiate(*this);
    if (!instance)
        return std::unexpected(instance.error());
    _instance = std::move(*instance);

    auto const lookup = [&](std::string_view name) {
        return _instance->function(name);
    };
    auto const load = lookup("load");
    auto const update = lookup("update");
    auto const render = lookup("render");
    auto const realloc = lookup("cabi_realloc");
    // The world check found them; a runtime that cannot is one that cannot instantiate the module.
    if (!load || !update || !render || !realloc)
        return std::unexpected(ExtensionError::InvalidComponent);
    _exports = Exports { .load = *load,
                         .update = *update,
                         .render = *render,
                         .realloc = *realloc,
                         .postLoad = lookup("cabi_post_load"),
                         .postUpdate = lookup("cabi_post_update"),
                         .postRender = lookup("cabi_post_render"),
                         .initialize = lookup("_initialize") };
    if (!_exports.initialize)
        return {};
    return guarded(_limits.loadDeadline, [&] { return callExport(*_exports.initialize, {}); });
}

WatchToken Instance::watchToken() noexcept
{
    return WatchToken { .instance = _instance.get(), .state = &_deadlineState };
}

bool Instance::isDeadlineExpired() const noexcept
{
    return _deadlineState.load(std::memory_order_acquire) == DeadlineState::Expired;
}

template <typename Body>
auto Instance::guarded(std::chrono::milliseconds timeout, Body const& body) -> decltype(body())
{
    Require(_engine->backend().isCurrentThreadPrepared());
    if (_fault)
        return std::unexpected(*_fault);

    _hostCallError.reset();
    _memory->resetLiftBudget();
    _deadline = _clock.now() + timeout;
    // Disarmed on every way out of the body, an exception included, which becomes a fault: no
    // deadline may outlive the call, or the watchdog could later interrupt a destroyed instance.
    auto result = runGuarded(*_engine, watchToken(), timeout, body);

    if (!result)
        _fault = result.error();
    else if (isDeadlineExpired())
        // The deadline expired after the guest returned, while the host was still lifting the result:
        // the call overran all the same, and the instance is interrupted.
        _fault = ExtensionError::DeadlineExceeded;

    if (_fault)
        return std::unexpected(*_fault);
    return result;
}

std::expected<void, ExtensionError> Instance::callExport(wasm::ExportFunction function,
                                                         std::span<uint32_t> argv)
{
    return _instance->call(function, argv).transform_error([this](wasm::CallFailure failure) {
        return guestFailure(failure);
    });
}

std::expected<void, ExtensionError> Instance::callPostReturn(OptionalFunction function, uint32_t result)
{
    if (!function)
        return {};
    auto argv = std::array { result };
    return callExport(*function, argv);
}

ExtensionError Instance::guestFailure(wasm::CallFailure failure) const noexcept
{
    if (_hostCallError)
        return *_hostCallError;
    if (isDeadlineExpired())
        return ExtensionError::DeadlineExceeded;
    // Refused growth is seen only as the reason this very call failed. Growth refused in a call that
    // returned (a guest allocator that copes with `memory.grow` failing) is not reported by the runtime
    // interface, so a later failure in the same guarded body is `Trapped`, where WAMR driven directly
    // once said `OutOfMemory` -- a documented deviation, in the label only. Refusal inside a host call's
    // allocation does reach here as `OutOfMemory`: the host call records it as its error, which ranks first.
    if (failure == wasm::CallFailure::MemoryGrowthRefused)
        return ExtensionError::OutOfMemory;
    return ExtensionError::Trapped;
}

std::unexpected<ExtensionError> Instance::failHostCall(ExtensionError error) noexcept
{
    if (!_hostCallError)
        _hostCallError = error;
    return std::unexpected(error);
}

std::expected<void, ExtensionError> Instance::callHost(std::size_t functionIndex,
                                                       std::span<uint32_t const> arguments) noexcept
{
    // The runtime calls only what it was created with, each with its arity; anything else is a guest
    // the host cannot serve.
    if (functionIndex >= HostCalls::Handlers.size()
        || arguments.size() != HostFunctions[functionIndex].parameterCount)
        return failHostCall(ExtensionError::Trapped);
    // Host calls do not nest. The only way into one from inside another is the guest's `cabi_realloc`,
    // which the host calls while it stores a result; a guest that makes a host call from there would
    // run host code against a half-stored result.
    if (_hostCall == HostCallState::InProgress)
        return failHostCall(ExtensionError::Trapped);

    // Called from inside the runtime: no exception may leave. One that a host function throws (the host
    // could not allocate, a host service failed) faults the call instead.
    _hostCall = HostCallState::InProgress;
    auto result = abi::Result<void> {};
    try
    {
        _memory->resetLiftBudget();
        result = HostCalls::Handlers[functionIndex](*this, arguments);
    }
    catch (...)
    {
        result = std::unexpected(ExtensionError::Trapped);
    }
    _hostCall = HostCallState::Idle;
    if (!result)
        return failHostCall(result.error());
    return {};
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
