// SPDX-License-Identifier: Apache-2.0
#include <vtextension/SessionExtensions.hpp>

#include <core/Assert.hpp>
#include <core/async/DetachedTask.hpp>
#include <core/async/ResumeOn.hpp>
#include <core/async/ThreadPoolExecutor.hpp>
#include <core/log/LogStore.hpp>

#include <ranges>
#include <thread>
#include <utility>

namespace vtextension
{

namespace
{
    /// What a build holds -- the engine, and the modules it loads -- released on an owner's thread however
    /// the build ends: taken by the owner when it is handed back, else retired to the sink wherever the
    /// build is dropped (the build thread, a frame its executor discards), never released there.
    class BuildResult
    {
      public:
        BuildResult(IRetirementSink& sink, std::expected<std::shared_ptr<Engine>, ExtensionError> engine):
            _sink { sink }, _engine { std::move(engine) }
        {
        }

        ~BuildResult()
        {
            _prepared.engine = std::move(_engine).value_or(_prepared.engine);
            if (_prepared.engine || std::ranges::any_of(_prepared.extensions, [](auto const& extension) {
                    return extension.compiled && *extension.compiled;
                }))
                _sink.retire(retirementOf(std::move(_prepared)));
        }

        BuildResult(BuildResult const&) = delete;
        BuildResult(BuildResult&&) = delete;
        BuildResult& operator=(BuildResult const&) = delete;
        BuildResult& operator=(BuildResult&&) = delete;

        /// Loads @p config's modules on the engine the build holds. On the build thread.
        void prepare(ExtensionHostConfig config)
        {
            _prepared = prepareExtensions(std::move(config), _engine);
            // The prepared extensions hold the engine now, if there is one.
            _engine = std::unexpected(ExtensionError::RuntimeUnavailable);
        }

        /// @return What was built, which the result no longer holds. On the owner's thread.
        [[nodiscard]] PreparedExtensions take() { return std::exchange(_prepared, {}); }

      private:
        IRetirementSink& _sink;
        std::expected<std::shared_ptr<Engine>, ExtensionError> _engine;
        PreparedExtensions _prepared;
    };

    /// Finds, reads and loads a pane's modules on the build executor, and hands them back to the owner's
    /// thread. By-value parameters: the frame outlives the call that starts it. It holds nothing of the pane
    /// but the owner thread's handle; @p adopt runs only on the owner's thread, and only while the pane
    /// lives.
    core::async::DetachedTask runBuild(DeclaredExtensions declared,
                                       vtbackend::ImageSize cellPixelSize,
                                       SessionExtensionServices services,
                                       std::shared_ptr<BuildResult> result,
                                       std::shared_ptr<IOwnerThread> owner,
                                       std::function<void(PreparedExtensions)> adopt)
    {
        co_await core::async::ResumeOn { services.builds };
        auto composed = composeExtensions(declared, services.lookup, cellPixelSize, services.limits);
        for (auto const& diagnostic: composed.diagnostics)
            errorLog()("{}", diagnostic);
        result->prepare(std::move(composed.host));
        owner->post([result = std::move(result), adopt = std::move(adopt)]() { adopt(result->take()); });
    }
} // namespace

ReplaceableStatusLineExtensions::ReplaceableStatusLineExtensions(std::shared_ptr<HostedExtensions> initial):
    _owned { std::move(initial) }
{
    _current.store(_owned);
}

std::shared_ptr<HostedExtensions> ReplaceableStatusLineExtensions::replace(
    std::shared_ptr<HostedExtensions> next)
{
    auto previous = std::exchange(_owned, next);
    _current.store(std::move(next));
    // From here on no call can reach `previous`; the ones already in it each hold a reference, for as long as
    // one non-blocking call takes. Once only this one is left, the caller holds the last.
    if (previous)
        while (previous.use_count() > 1)
            std::this_thread::yield();
    return previous;
}

vtbackend::StatusLineEventKinds ReplaceableStatusLineExtensions::subscriptions() const noexcept
{
    auto const current = _current.load();
    return current ? current->extensions().subscriptions() : vtbackend::StatusLineEventKinds {};
}

void ReplaceableStatusLineExtensions::post(vtbackend::StatusLineEvent event) noexcept
{
    if (auto const current = _current.load())
        current->extensions().post(std::move(event));
}

void ReplaceableStatusLineExtensions::resize(vtbackend::PageSize statusLineSize) noexcept
{
    if (auto const current = _current.load())
        current->extensions().resize(statusLineSize);
}

std::shared_ptr<vtbackend::StatusLineExtensionSnapshot const> ReplaceableStatusLineExtensions::snapshot(
    std::string_view name) const noexcept
{
    auto const current = _current.load();
    return current ? current->extensions().snapshot(name) : nullptr;
}

std::optional<std::string> ReplaceableStatusLineExtensions::takeOverName() const noexcept
{
    auto const current = _current.load();
    return current ? current->extensions().takeOverName() : std::nullopt;
}

SessionExtensions::SessionExtensions(SessionExtensionCallbacks callbacks,
                                     std::shared_ptr<IOwnerThread> owner):
    _callbacks { std::move(callbacks) }, _owner { std::move(owner) }
{
}

SessionExtensions::~SessionExtensions()
{
    // The terminal is gone (its destructor revoked _published), so nothing calls the forwarder any more: the
    // extensions are destroyed here, on the owner's thread, before the sink they publish through.
    (void) _forwarder.replace(nullptr);
}

void SessionExtensions::rebuild(DeclaredExtensions declared,
                                vtbackend::ImageSize cellPixelSize,
                                std::optional<SessionExtensionServices> services)
{
    auto const generation = ++_generation;
    if (declared.extensions.empty() || !services)
    {
        if (!_forwarder.empty())
            install(nullptr);
        return;
    }
    // Acquired here, on the owner's thread, which alone may bring the runtime up (and so tear it down); the
    // build only loads modules on it.
    auto result = std::make_shared<BuildResult>(services->retirement, services->engines.acquire());
    runBuild(std::move(declared),
             cellPixelSize,
             *services,
             std::move(result),
             _owner,
             [this, generation, services = *services](PreparedExtensions prepared) {
                 adopt(generation, std::move(prepared), services);
             });
}

void SessionExtensions::adopt(uint64_t generation,
                              PreparedExtensions prepared,
                              SessionExtensionServices services)
{
    // A later rebuild superseded this one: what it built is released here, on the owner's thread.
    if (generation != _generation)
        return;
    install(std::make_shared<HostedExtensions>(
        std::move(prepared),
        ExtensionHostServices { .engines = services.engines,
                                .runners =
                                    RunnerServices { .scheduler = services.scheduler,
                                                     .grants = services.grants,
                                                     .capture = services.capture,
                                                     .wallClock = services.wallClock,
                                                     .localTime = services.localTime,
                                                     .environment = services.environment,
                                                     .fileSystem = services.fileSystem,
                                                     .retirement = services.retirement,
                                                     .onSnapshotPublished = publisherFor(_published),
                                                     .onPermissionNeeded = _callbacks.onPermissionNeeded,
                                                     .onLog = _callbacks.onLog,
                                                     .callControl = _callbacks.callControl } }));
}

void SessionExtensions::install(std::shared_ptr<HostedExtensions> next)
{
    // Published first, then the terminal reads the new take-over and subscriptions, then the old extensions
    // are destroyed, holding no lock: none of their runners waits for its extension, only for a callback in
    // flight, which takes no lock of the owner's.
    auto previous = _forwarder.replace(std::move(next));
    if (_callbacks.onReplaced)
        _callbacks.onReplaced();
    previous.reset();
}

ExtensionRuntime::ExtensionRuntime(ExtensionRuntimeConfig config, ExtensionRuntimeServices services):
    _services { services },
    _bundledDirectories { std::move(config.bundledDirectories) },
    _limits { config.limits },
    _grants { services.fileSystem, std::move(config.grantsFile) },
    _engines { services.backend, config.limits },
    _pool { services.backend, config.workerCount },
    _builds { std::make_unique<core::async::ThreadPoolExecutor>(1) }
{
}

SessionExtensionServices ExtensionRuntime::sessionServices() noexcept
{
    Require(_builds != nullptr); // not after stop()
    return { .engines = _engines,
             .retirement = _engines,
             .scheduler = _pool,
             .builds = *_builds,
             .grants = _grants,
             .capture = _services.capture,
             .wallClock = _services.wallClock,
             .localTime = _services.localTime,
             .environment = _services.environment,
             .fileSystem = _services.fileSystem,
             .lookup = ExtensionModuleLookup { .fileSystem = _services.fileSystem,
                                               .bundledDirectories = _bundledDirectories },
             .limits = _limits };
}

} // namespace vtextension
