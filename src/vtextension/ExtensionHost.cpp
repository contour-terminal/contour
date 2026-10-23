// SPDX-License-Identifier: Apache-2.0
#include <vtextension/ExtensionHost.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

#include <vtextension/Surface.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>

namespace vtextension
{

namespace
{
    /// @return How many cells @p text takes, counting one per code point: enough for a notice.
    [[nodiscard]] int codepointCount(std::string_view text) noexcept
    {
        return static_cast<int>(std::ranges::count_if(
            text, [](char c) { return (static_cast<unsigned char>(c) & 0xC0U) != 0x80U; }));
    }
} // namespace

EngineProvider::~EngineProvider()
{
    releaseRetired();
}

std::unique_ptr<RetiredExtension> EngineProvider::takeRetired() noexcept
{
    auto const lock = std::scoped_lock { _mutex };
    return std::move(_retired);
}

void EngineProvider::release(std::unique_ptr<RetiredExtension> list) noexcept
{
    // One at a time rather than by the chain's recursive destructor, however long the list.
    while (list)
        list = std::move(list->next);
}

void EngineProvider::releaseRetired() noexcept
{
    // Released here, outside the lock: destroying an engine joins its watchdog.
    release(takeRetired());
}

void EngineProvider::retire(std::unique_ptr<RetiredExtension> retired) noexcept
{
    // Allocates nothing: the runner allocated the node on its owner's thread, and it links in here.
    auto const lock = std::scoped_lock { _mutex };
    retired->next = std::move(_retired);
    _retired = std::move(retired);
}

std::expected<std::shared_ptr<Engine>, ExtensionError> EngineProvider::acquire()
{
    // Taken out first and released last, after the cache is consulted, so that a retired engine
    // still alive is reused rather than torn down and created again.
    auto retired = takeRetired();
    auto engine = [&] {
        auto const lock = std::scoped_lock { _mutex };
        if (auto cached = _engine.lock())
            return std::expected<std::shared_ptr<Engine>, ExtensionError> { std::move(cached) };
        return Engine::create(_backend, _limits).transform([&](std::shared_ptr<Engine> created) {
            _engine = created;
            return created;
        });
    }();
    release(std::move(retired));
    return engine;
}

PreparedExtensions prepareExtensions(ExtensionHostConfig config,
                                     std::expected<std::shared_ptr<Engine>, ExtensionError> engine)
{
    auto prepared = PreparedExtensions { .extensions = {},
                                         .engine = engine.value_or(nullptr),
                                         .cellPixelSize = config.cellPixelSize,
                                         .limits = config.limits };
    // Entered only if a module is to be loaded, and so only on a thread that may: the thread that brought
    // the runtime up already is, and a guard there does nothing.
    auto threadGuard = std::optional<std::expected<std::unique_ptr<wasm::IThreadGuard>, ExtensionError>> {};
    prepared.extensions.reserve(config.extensions.size());
    for (auto& spec: config.extensions)
    {
        auto compiled = [&]() -> std::expected<std::shared_ptr<CompiledExtension>, ExtensionError> {
            if (spec.module.empty())
                return std::unexpected(spec.moduleError);
            if (!engine)
                return std::unexpected(engine.error());
            if (!threadGuard)
                threadGuard.emplace((*engine)->backend().enterThread());
            if (!*threadGuard)
                return std::unexpected(threadGuard->error());
            return CompiledExtension::load(**engine, std::move(spec.module));
        }();
        spec.module.clear();
        prepared.extensions.push_back(
            PreparedExtension { .spec = std::move(spec), .compiled = std::move(compiled) });
    }
    return prepared;
}

std::unique_ptr<RetiredExtension> retirementOf(PreparedExtensions prepared)
{
    auto list = std::make_unique<RetiredExtension>();
    list->engine = std::move(prepared.engine);
    for (auto& extension: prepared.extensions)
        if (extension.compiled && *extension.compiled)
        {
            auto node = std::make_unique<RetiredExtension>();
            node->compiled = std::move(*extension.compiled);
            node->next = std::move(list);
            list = std::move(node);
        }
    return list;
}

namespace
{
    /// @return Whether any extension of @p config has module bytes to load.
    [[nodiscard]] bool hasModules(ExtensionHostConfig const& config) noexcept
    {
        return std::ranges::any_of(config.extensions,
                                   [](ExtensionSpec const& spec) { return !spec.module.empty(); });
    }
} // namespace

ExtensionHost::ExtensionHost(ExtensionHostConfig config, ExtensionHostServices services):
    ExtensionHost(
        [&] {
            // Acquired only for a module there is, so that extensions whose modules are all missing start no
            // runtime either.
            auto engine = hasModules(config) ? services.engines.acquire()
                                             : std::expected<std::shared_ptr<Engine>, ExtensionError> {
                                                   std::unexpected(ExtensionError::ModuleUnavailable)
                                               };
            return prepareExtensions(std::move(config), std::move(engine));
        }(),
        services)
{
}

ExtensionHost::ExtensionHost(PreparedExtensions prepared, ExtensionHostServices services):
    _engines { services.engines }
{
    _entries.reserve(prepared.extensions.size());
    for (auto& [spec, compiled]: prepared.extensions)
    {
        if (!compiled)
        {
            // Drawn as the Runner draws its own notice, so that a take-over status line shows it too.
            auto const text = disabledNotice(spec.name, compiled.error());
            auto surface = Surface(
                SurfaceConfig { .pageSize = vtbackend::PageSize { .lines = vtbackend::LineCount(1),
                                                                  .columns = vtbackend::ColumnCount(
                                                                      std::max(codepointCount(text), 1)) },
                                .cellPixelSize = prepared.cellPixelSize,
                                .replyCapacity = prepared.limits.maxReplyBytes },
                SurfaceServices { .environment = services.runners.environment,
                                  .fileSystem = services.runners.fileSystem });
            _entries.push_back(Entry {
                .name = spec.name,
                .placement = spec.placement,
                .runner = nullptr,
                .notice = std::make_shared<StatusLineSnapshot const>(drawnNotice(text, surface, 1)) });
            continue;
        }
        auto runner = std::make_unique<Runner>(prepared.engine,
                                               std::move(*compiled),
                                               RunnerConfig { .name = spec.name,
                                                              .hostId = std::move(spec.hostId),
                                                              .config = std::move(spec.config),
                                                              .placement = spec.placement,
                                                              .runCommand = spec.runCommand,
                                                              .cellPixelSize = prepared.cellPixelSize,
                                                              .limits = prepared.limits },
                                               services.runners);
        _entries.push_back(Entry { .name = std::move(spec.name),
                                   .placement = spec.placement,
                                   .runner = std::move(runner),
                                   .notice = nullptr });
    }
}

ExtensionHost::~ExtensionHost()
{
    _entries.clear();
    _engines.releaseRetired();
}

EventKinds ExtensionHost::subscriptions() const noexcept
{
    auto kinds = EventKinds {};
    for (auto const& entry: _entries)
        if (entry.runner)
            kinds |= entry.runner->subscriptions();
    return kinds;
}

uint64_t ExtensionHost::dropCount() const noexcept
{
    auto count = uint64_t { 0 };
    for (auto const& entry: _entries)
        if (entry.runner)
            count += entry.runner->dropCount();
    return count;
}

void ExtensionHost::post(Event const& event) noexcept
{
    for (auto const& entry: _entries)
        if (entry.runner)
            entry.runner->post(event);
}

void ExtensionHost::resize(vtbackend::PageSize size) noexcept
{
    for (auto const& entry: _entries)
        if (entry.runner)
            entry.runner->resize(size);
}

std::shared_ptr<StatusLineSnapshot const> ExtensionHost::snapshot(std::string_view name) const noexcept
{
    auto const entry = std::ranges::find(_entries, name, &Entry::name);
    if (entry == _entries.end())
        return nullptr;
    return entry->runner ? entry->runner->latest() : entry->notice;
}

std::optional<std::string> ExtensionHost::takeOverName() const
{
    auto const entry = std::ranges::find(_entries, Placement::TakeOver, &Entry::placement);
    if (entry == _entries.end())
        return std::nullopt;
    return entry->name;
}

} // namespace vtextension
