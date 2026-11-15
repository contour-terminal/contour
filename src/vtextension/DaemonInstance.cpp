// SPDX-License-Identifier: Apache-2.0
#include <vtextension/DaemonInstance.hpp>

#include <core/Assert.hpp>
#include <core/platform/Clock.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <vtextension/CanonicalAbi.hpp>
#include <vtextension/ControlImports.hpp>
#include <vtextension/DaemonImports.hpp>
#include <vtextension/World.hpp>

namespace vtextension
{

namespace
{
    static_assert(abi::Codec<std::string>::FlatCount + abi::Codec<DaemonHostInfo>::FlatCount
                      <= MaxFlatParameters,
                  "load takes its configuration and the host's information flat");
} // namespace

/// The daemon world's imports: each receives the core-wasm parameters the Canonical ABI flattens its WIT
/// signature to (DaemonImportRow::parameterCount), lifts them, calls the IDaemonHostServices, and stores
/// the answer at the result address the guest passes last. A guest that breaks the ABI (an address
/// outside its memory, a string that is not UTF-8, an unknown enum case) traps.
struct DaemonInstance::HostCalls
{
    /// What runs one import, given its flat arguments (as many as its row says).
    using Handler = abi::Result<void> (*)(DaemonInstance& instance, std::span<uint32_t const> arguments);

    /// Lifts the next flat value of type @p T.
    template <typename T>
    [[nodiscard]] static abi::Result<T> lift(DaemonInstance& instance, abi::FlatReader& reader)
    {
        return abi::Codec<T>::liftFlat(instance._guest.memory(), reader);
    }

    /// Stores @p value at the result address @p arguments ends with.
    template <typename T>
    [[nodiscard]] static abi::Result<void> answer(DaemonInstance& instance,
                                                  std::span<uint32_t const> arguments,
                                                  T const& value)
    {
        return abi::Codec<T>::store(instance._guest.memory(), arguments.back(), value);
    }

    /// `log: func(level: log-level, message: string)`.
    static abi::Result<void> log(DaemonInstance& instance, std::span<uint32_t const> arguments)
    {
        auto reader = abi::FlatReader(arguments);
        auto const level = lift<LogLevel>(instance, reader);
        if (!level)
            return std::unexpected(level.error());
        return lift<std::string>(instance, reader).transform([&](std::string const& message) {
            instance._services.log(*level, message);
        });
    }

    /// `wake: func(after-ms: u32)`.
    static abi::Result<void> wake(DaemonInstance& instance, std::span<uint32_t const> arguments)
    {
        instance._services.wake(std::chrono::milliseconds { arguments[0] });
        return {};
    }

    /// `now: func() -> clock-reading`.
    static abi::Result<void> now(DaemonInstance& instance, std::span<uint32_t const> arguments)
    {
        return answer(instance, arguments, instance._services.now());
    }

    /// `proc-start: func(command: string, args: list<tuple<string, string>>, cwd: string, tag: string)
    /// -> result<proc-handle, host-error>`.
    static abi::Result<void> procStart(DaemonInstance& instance, std::span<uint32_t const> arguments)
    {
        auto reader = abi::FlatReader(arguments.first(arguments.size() - 1));
        auto const command = lift<std::string>(instance, reader);
        if (!command)
            return std::unexpected(command.error());
        auto const args = lift<ConfigMap>(instance, reader);
        if (!args)
            return std::unexpected(args.error());
        auto const cwd = lift<std::string>(instance, reader);
        if (!cwd)
            return std::unexpected(cwd.error());
        auto const tag = lift<std::string>(instance, reader);
        if (!tag)
            return std::unexpected(tag.error());
        return answer(instance, arguments, instance._services.procStart(*command, *args, *cwd, *tag));
    }

    /// `procs: func() -> result<list<proc-info>, host-error>`.
    static abi::Result<void> procs(DaemonInstance& instance, std::span<uint32_t const> arguments)
    {
        return answer(instance, arguments, instance._services.procs());
    }

    /// `proc-take: func(handle: proc-handle, output: proc-stream, offset: u32) -> result<chunk, host-error>`.
    static abi::Result<void> procTake(DaemonInstance& instance, std::span<uint32_t const> arguments)
    {
        auto reader = abi::FlatReader(arguments.subspan(1, 1));
        auto const stream = lift<ProcStream>(instance, reader);
        if (!stream)
            return std::unexpected(stream.error());
        return answer(instance, arguments, instance._services.procTake(arguments[0], *stream, arguments[2]));
    }

    /// `proc-release: func(handle: proc-handle) -> result<_, host-error>`.
    static abi::Result<void> procRelease(DaemonInstance& instance, std::span<uint32_t const> arguments)
    {
        return answer(instance, arguments, instance._services.procRelease(arguments[0]));
    }

    /// `store-get: func(key: string) -> result<option<list<u8>>, host-error>`.
    static abi::Result<void> storeGet(DaemonInstance& instance, std::span<uint32_t const> arguments)
    {
        auto reader = abi::FlatReader(arguments);
        auto const key = lift<std::string>(instance, reader);
        if (!key)
            return std::unexpected(key.error());
        return answer(instance, arguments, instance._services.storeGet(*key));
    }

    /// `store-set: func(key: string, value: list<u8>) -> result<_, host-error>`, with the deadline paused.
    static abi::Result<void> storeSet(DaemonInstance& instance, std::span<uint32_t const> arguments)
    {
        auto reader = abi::FlatReader(arguments);
        auto const key = lift<std::string>(instance, reader);
        if (!key)
            return std::unexpected(key.error());
        auto const value = lift<ByteList>(instance, reader);
        if (!value)
            return std::unexpected(value.error());
        auto const result =
            instance._guest.pauseDeadline([&] { return instance._services.storeSet(*key, *value); });
        if (!result)
            return std::unexpected(result.error());
        return answer(instance, arguments, *result);
    }

    /// `store-remove: func(key: string) -> result<_, host-error>`, with the deadline paused.
    static abi::Result<void> storeRemove(DaemonInstance& instance, std::span<uint32_t const> arguments)
    {
        auto reader = abi::FlatReader(arguments);
        auto const key = lift<std::string>(instance, reader);
        if (!key)
            return std::unexpected(key.error());
        auto const result =
            instance._guest.pauseDeadline([&] { return instance._services.storeRemove(*key); });
        if (!result)
            return std::unexpected(result.error());
        return answer(instance, arguments, *result);
    }

    /// `store-keys: func(prefix: string) -> result<list<string>, host-error>`.
    static abi::Result<void> storeKeys(DaemonInstance& instance, std::span<uint32_t const> arguments)
    {
        auto reader = abi::FlatReader(arguments);
        auto const prefix = lift<std::string>(instance, reader);
        if (!prefix)
            return std::unexpected(prefix.error());
        return answer(instance, arguments, instance._services.storeKeys(*prefix));
    }

    /// `summary-set: func(short: string, detail: string) -> result<_, host-error>`.
    static abi::Result<void> summarySet(DaemonInstance& instance, std::span<uint32_t const> arguments)
    {
        auto reader = abi::FlatReader(arguments);
        auto const shortText = lift<std::string>(instance, reader);
        if (!shortText)
            return std::unexpected(shortText.error());
        auto const detail = lift<std::string>(instance, reader);
        if (!detail)
            return std::unexpected(detail.error());
        return answer(instance, arguments, instance._services.summarySet(*shortText, *detail));
    }

    /// An import this host does not serve yet, whose WIT result is @p Result: answers its error type's
    /// `not-supported-yet` at the result address, lifting nothing and calling no service. The result type
    /// fixes where the error lands: a result's payload is aligned to its widest case.
    template <typename Result>
    static abi::Result<void> notSupportedYet(DaemonInstance& instance, std::span<uint32_t const> arguments)
    {
        return answer(instance, arguments, Result { std::unexpect, Result::error_type::NotSupportedYet });
    }

    /// The results of the imports not served yet.
    using StringResult = std::expected<std::string, HostError>;
    using UnitResult = std::expected<void, HostError>;
    using PanesResult = std::expected<std::vector<PaneInfo>, HostError>;
    using TabsResult = std::expected<std::vector<TabInfo>, HostError>;
    using BytesResult = std::expected<ByteList, HostError>;
    using ActionsResult = std::expected<std::vector<ActionInfo>, HostError>;
    using FileResult = std::expected<FileContent, FileError>;

    /// What runs each import, indexed by DaemonImport.
    static constexpr auto Handlers = std::array<Handler, DaemonImportRows.size()> {
        &log,                            // log
        &wake,                           // wake
        &now,                            // now
        &procStart,                      // proc-start
        &procs,                          // procs
        &procTake,                       // proc-take
        &procRelease,                    // proc-release
        &storeGet,                       // store-get
        &storeSet,                       // store-set
        &storeRemove,                    // store-remove
        &storeKeys,                      // store-keys
        &notSupportedYet<StringResult>,  // pane-open
        &notSupportedYet<UnitResult>,    // pane-close
        &notSupportedYet<PanesResult>,   // panes
        &notSupportedYet<StringResult>,  // tab-open
        &notSupportedYet<UnitResult>,    // tab-close
        &notSupportedYet<UnitResult>,    // tab-reveal
        &notSupportedYet<TabsResult>,    // tabs
        &notSupportedYet<BytesResult>,   // tab-read
        &notSupportedYet<UnitResult>,    // tab-write
        &notSupportedYet<ActionsResult>, // actions-take
        &summarySet,                     // summary-set
        &notSupportedYet<UnitResult>,    // pane-reveal
        &notSupportedYet<FileResult>,    // file-read
    };

    /// Every handler that answers `not-supported-yet`.
    static constexpr auto NotSupportedYetHandlers = std::array<Handler, 7> {
        &notSupportedYet<StringResult>, &notSupportedYet<UnitResult>,  &notSupportedYet<PanesResult>,
        &notSupportedYet<TabsResult>,   &notSupportedYet<BytesResult>, &notSupportedYet<ActionsResult>,
        &notSupportedYet<FileResult>,
    };

    /// @return Whether the handler of @p row answers exactly as the row's availability says.
    [[nodiscard]] static constexpr bool matchesAvailability(DaemonImportRow const& row)
    {
        auto const handler = Handlers.at(std::to_underlying(row.import));
        auto const answersNotSupportedYet = std::ranges::contains(NotSupportedYetHandlers, handler);
        return answersNotSupportedYet == (row.availability == ImportAvailability::NotSupportedYet);
    }
};

DaemonInstance::DaemonInstance(PrivateTag /*unused*/,
                               std::shared_ptr<CompiledExtension const> compiled,
                               IDaemonHostServices& services,
                               core::platform::IClock& clock,
                               IDeadlineWatchdog& watchdog,
                               ExtensionLimits limits):
    _compiled { std::move(compiled) },
    _services { services },
    _limits { limits },
    _guest { _compiled->engine().backend(), clock, watchdog, _limits }
{
}

DaemonInstance::~DaemonInstance() = default;

std::expected<std::unique_ptr<DaemonInstance>, ExtensionError> DaemonInstance::create(
    std::shared_ptr<CompiledExtension const> compiled,
    IDaemonHostServices& services,
    core::platform::IClock& clock,
    IDeadlineWatchdog& watchdog,
    ExtensionLimits limits)
{
    Require(compiled != nullptr && compiled->engine().world() == World::Daemon);
    // The memory and stack caps are the engine's runtime's: an instance cannot be held to others.
    auto const& engineLimits = compiled->engine().limits();
    Require(limits.memoryBytes == engineLimits.memoryBytes && limits.stackBytes == engineLimits.stackBytes);
    auto instance = std::make_unique<DaemonInstance>(
        PrivateTag {}, std::move(compiled), services, clock, watchdog, limits);
    return instance->instantiate().transform([&] { return std::move(instance); });
}

std::expected<void, ExtensionError> DaemonInstance::instantiate()
{
    if (auto instantiated = _guest.instantiate(_compiled->module(), *this); !instantiated)
        return instantiated;
    auto const load = _guest.function("load");
    auto const update = _guest.function("update");
    // The world check found them; a runtime that cannot is one that cannot instantiate the module.
    if (!load || !update)
        return std::unexpected(ExtensionError::InvalidComponent);
    _exports = Exports { .load = *load, .update = *update };
    return _guest.initialize();
}

std::expected<void, ExtensionError> DaemonInstance::callHost(std::size_t functionIndex,
                                                             std::span<uint32_t const> arguments) noexcept
{
    // Flipping a row to Served without giving it a handler, or serving a row still declared
    // NotSupportedYet, does not compile.
    static_assert(std::ranges::all_of(DaemonImportRows, &HostCalls::matchesAvailability),
                  "a daemon import is served exactly when its row says so");

    return _guest.dispatch(functionIndex, arguments, daemonHostFunctions(), [&](std::size_t index) {
        return HostCalls::Handlers.at(index)(*this, arguments);
    });
}

std::expected<void, ExtensionError> DaemonInstance::load(std::string_view configJson,
                                                         DaemonHostInfo const& info)
{
    return _guest.guarded(_limits.loadDeadline, [&]() -> std::expected<void, ExtensionError> {
        auto argv = abi::FlatValues {};
        if (auto lowered = abi::Codec<std::string>::lowerFlat(_guest.memory(), std::string(configJson), argv);
            !lowered)
            return std::unexpected(lowered.error());
        if (auto lowered = abi::Codec<DaemonHostInfo>::lowerFlat(_guest.memory(), info, argv); !lowered)
            return std::unexpected(lowered.error());
        return _guest.callExport(_exports.load, argv);
    });
}

std::expected<void, ExtensionError> DaemonInstance::update(DaemonHint hint)
{
    return _guest.guarded(_limits.callDeadline, [&]() -> std::expected<void, ExtensionError> {
        auto argv = abi::FlatValues {};
        if (auto lowered = abi::Codec<DaemonHint>::lowerFlat(_guest.memory(), hint, argv); !lowered)
            return std::unexpected(lowered.error());
        return _guest.callExport(_exports.update, argv);
    });
}

} // namespace vtextension
