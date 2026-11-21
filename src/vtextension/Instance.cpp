// SPDX-License-Identifier: Apache-2.0
#include <vtextension/Instance.hpp>

#include <core/Assert.hpp>

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
#include <vtextension/ControlImports.hpp>
#include <vtextension/HostBindings.hpp>
#include <vtextension/StatuslineWorld.hpp>
#include <vtextension/World.hpp>

namespace vtextension
{

namespace
{
    static_assert(abi::Codec<Event>::FlatCount <= MaxFlatParameters, "update takes its event flat");
    static_assert(abi::Codec<ConfigMap>::FlatCount <= MaxFlatParameters, "load takes its config flat");

    /// @return @p extent as a cell count of the world's `u32`; negative extents count as zero.
    [[nodiscard]] uint32_t cellCount(int extent) noexcept
    {
        return static_cast<uint32_t>(std::max(extent, 0));
    }

    /// The statusline world's host functions: the first rows of the table every runtime is created with.
    constexpr auto StatuslineHostFunctions = std::array {
        wasm::HostFunction {
            .module = detail::HostImportModule, .name = "request-frame", .parameterCount = 1 },
        wasm::HostFunction { .module = detail::HostImportModule, .name = "log", .parameterCount = 3 },
        wasm::HostFunction { .module = detail::HostImportModule, .name = "state", .parameterCount = 1 },
        wasm::HostFunction { .module = detail::HostImportModule, .name = "run-command", .parameterCount = 5 },
    };

    /// How many host functions there are: the statusline world's, then the control interface's.
    constexpr auto HostFunctionCount = StatuslineHostFunctions.size() + ControlImportCount;

    /// The table every runtime is created with: the index of each row is what the runtime calls it by, and
    /// the row of Instance's handler table that runs it.
    [[nodiscard]] std::array<wasm::HostFunction, HostFunctionCount> const& hostFunctionTable() noexcept
    {
        static auto const table = [] {
            auto rows = std::array<wasm::HostFunction, HostFunctionCount> {};
            auto const next = std::ranges::copy(StatuslineHostFunctions, rows.begin()).out;
            std::ranges::copy(controlHostFunctions(), next);
            return rows;
        }();
        return table;
    }

    /// @return The control permissions @p compiled's manifest declares; none without a manifest.
    [[nodiscard]] vtcontrol::ControlGrants declaredControlOf(CompiledExtension const& compiled) noexcept
    {
        return compiled.manifest() ? controlGrantsOf(compiled.manifest()->control)
                                   : vtcontrol::ControlGrants::none();
    }
} // namespace

namespace detail
{
    std::span<wasm::HostFunction const> statuslineHostFunctions() noexcept
    {
        return hostFunctionTable();
    }
} // namespace detail

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
        auto const lifted = abi::Codec<LogLevel>::liftFlat(instance._guest.memory(), reader);
        if (!lifted)
            return std::unexpected(lifted.error());
        return abi::Codec<std::string>::liftFlat(instance._guest.memory(), reader)
            .transform([&](std::string const& message) { instance._services.log(*lifted, message); });
    }

    /// `state: func() -> state-snapshot`, whose result the guest receives at the address it passes.
    static abi::Result<void> state(Instance& instance, std::span<uint32_t const> arguments)
    {
        return abi::Codec<StateSnapshot>::store(
            instance._guest.memory(), arguments[0], instance._services.state());
    }

    /// `run-command: func(name: string, args: list<tuple<string, string>>) ->
    /// result<command-output, command-error>`, whose result the guest receives at the address it passes
    /// last.
    static abi::Result<void> runCommand(Instance& instance, std::span<uint32_t const> arguments)
    {
        auto& memory = instance._guest.memory();
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
        auto output =
            instance._guest.pauseDeadline([&] { return instance._services.runCommand(name, args); });
        if (!output)
            return std::unexpected(output.error());

        return std::move(*output)
            .transform([](std::string const& stdoutText) {
                // The host reports a finished command by its output; a command that did not exit
                // normally is an error, so one that finished counts as exit code 0.
                return abi::CommandOutput { .exitCode = 0,
                                            .standardOutput = ByteList(stdoutText.begin(), stdoutText.end()),
                                            .standardError = {} };
            })
            .transform_error([](ExtensionError error) { return abi::commandErrorFor(error); });
    }

    /// A control verb, the control interface's import @p Import: served by callControlImport() with the
    /// rest of the call's deadline, which keeps running meanwhile -- unlike a command's, the host bounds the
    /// verb by the guest's own time.
    template <std::size_t Import>
    static abi::Result<void> control(Instance& instance, std::span<uint32_t const> arguments)
    {
        auto const remaining = std::chrono::floor<std::chrono::milliseconds>(instance._guest.remaining());
        return callControlImport(
            Import,
            instance._guest.memory(),
            arguments,
            instance._services,
            instance._declaredControl,
            ControlCallBudget { .remaining = remaining, .deadline = instance._guest.deadlineState() });
    }

    /// What runs each host function, by its index in the host function table.
    static constexpr auto Handlers = []<std::size_t... Import>(std::index_sequence<Import...>) {
        return std::array<Handler, HostFunctionCount> {
            &requestFrame, &log, &state, &runCommand, &control<Import>...
        };
    }(std::make_index_sequence<ControlImportCount> {});
    static_assert(StatuslineHostFunctions.size() == 4, "a handler for every statusline host function");
};

Instance::Instance(PrivateTag /*unused*/,
                   std::shared_ptr<CompiledExtension const> compiled,
                   IHostServices& services,
                   ExtensionLimits limits):
    _compiled { std::move(compiled) },
    _engine { _compiled->engine().shared_from_this() },
    _services { services },
    _limits { limits },
    _declaredControl { declaredControlOf(*_compiled) },
    _guest { _engine->backend(), *_engine, _limits }
{
}

Instance::~Instance() = default;

std::expected<std::unique_ptr<Instance>, ExtensionError> Instance::create(Engine& engine,
                                                                          CompiledExtension const& compiled,
                                                                          IHostServices& services,
                                                                          ExtensionLimits limits)
{
    Require(&engine == &compiled.engine() && engine.world() == World::StatusLine);
    // The memory and stack caps are the engine's runtime's: an instance cannot be held to others.
    Require(limits.memoryBytes == engine.limits().memoryBytes
            && limits.stackBytes == engine.limits().stackBytes);
    auto instance = std::make_unique<Instance>(PrivateTag {}, compiled.shared_from_this(), services, limits);
    return instance->instantiate().transform([&] { return std::move(instance); });
}

std::expected<void, ExtensionError> Instance::instantiate()
{
    if (auto instantiated = _guest.instantiate(_compiled->module(), *this); !instantiated)
        return instantiated;
    auto const load = _guest.function("load");
    auto const update = _guest.function("update");
    auto const render = _guest.function("render");
    // The world check found them; a runtime that cannot is one that cannot instantiate the module.
    if (!load || !update || !render)
        return std::unexpected(ExtensionError::InvalidComponent);
    _exports = Exports { .load = *load,
                         .update = *update,
                         .render = *render,
                         .postLoad = _guest.function("cabi_post_load"),
                         .postUpdate = _guest.function("cabi_post_update"),
                         .postRender = _guest.function("cabi_post_render") };
    return _guest.initialize();
}

std::expected<void, ExtensionError> Instance::callPostReturn(OptionalFunction function, uint32_t result)
{
    if (!function)
        return {};
    auto argv = std::array { result };
    return _guest.callExport(*function, argv);
}

std::expected<void, ExtensionError> Instance::callHost(std::size_t functionIndex,
                                                       std::span<uint32_t const> arguments) noexcept
{
    return _guest.dispatch(functionIndex, arguments, hostFunctionTable(), [&](std::size_t index) {
        return HostCalls::Handlers.at(index)(*this, arguments);
    });
}

std::expected<EventKinds, ExtensionError> Instance::load(ConfigMap const& config)
{
    return _guest.guarded(_limits.loadDeadline, [&]() -> std::expected<EventKinds, ExtensionError> {
        auto argv = abi::FlatValues {};
        if (auto lowered = abi::Codec<ConfigMap>::lowerFlat(_guest.memory(), config, argv); !lowered)
            return std::unexpected(lowered.error());
        if (auto called = _guest.callExport(_exports.load, argv); !called)
            return std::unexpected(called.error());
        auto reader = abi::FlatReader(std::span { argv }.first(1));
        auto const kinds = abi::Codec<EventKinds>::liftFlat(_guest.memory(), reader);
        if (!kinds)
            return std::unexpected(kinds.error());
        return callPostReturn(_exports.postLoad, argv.front()).transform([&] {
            return (*kinds & SubscribableKinds) | AlwaysDelivered;
        });
    });
}

std::expected<RenderRequest, ExtensionError> Instance::update(Event const& event)
{
    return _guest.guarded(_limits.callDeadline, [&]() -> std::expected<RenderRequest, ExtensionError> {
        auto argv = abi::FlatValues {};
        if (auto lowered = abi::Codec<Event>::lowerFlat(_guest.memory(), event, argv); !lowered)
            return std::unexpected(lowered.error());
        if (auto called = _guest.callExport(_exports.update, argv); !called)
            return std::unexpected(called.error());
        auto reader = abi::FlatReader(std::span { argv }.first(1));
        auto const request = abi::Codec<RenderRequest>::liftFlat(_guest.memory(), reader);
        if (!request)
            return std::unexpected(request.error());
        return callPostReturn(_exports.postUpdate, argv.front()).transform([&] { return *request; });
    });
}

std::expected<std::string, ExtensionError> Instance::render(vtpty::PageSize size)
{
    return _guest.guarded(_limits.callDeadline, [&]() -> std::expected<std::string, ExtensionError> {
        auto argv = std::array { cellCount(unbox(size.columns)), cellCount(unbox(size.lines)) };
        if (auto called = _guest.callExport(_exports.render, argv); !called)
            return std::unexpected(called.error());
        auto const resultAddress = argv.front();
        auto bytes = abi::loadByteString(_guest.memory(), resultAddress, _limits.maxRenderBytes);
        if (!bytes)
            return std::unexpected(bytes.error());
        return callPostReturn(_exports.postRender, resultAddress).transform([&] {
            return std::move(*bytes);
        });
    });
}

} // namespace vtextension
