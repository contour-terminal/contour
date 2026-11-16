// SPDX-License-Identifier: Apache-2.0
#include <vtextension/DaemonExtensionRuntime.hpp>

#include <core/Utf8Path.hpp>
#include <core/log/LogStore.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <mutex>
#include <span>
#include <utility>

#include <vtextension/CompiledExtension.hpp>
#include <vtextension/DeclaredProcesses.hpp>
#include <vtextension/ExtensionComposition.hpp>
#include <vtextension/FileExtensionStore.hpp>
#include <vtextension/Manifest.hpp>
#include <vtextension/RootPointer.hpp>
#include <vtextension/World.hpp>

namespace vtextension
{

namespace
{
    /// One error's text, for a problem's message.
    template <typename Error>
    struct ErrorText
    {
        Error error;
        std::string_view text;
    };

    inline constexpr auto RootPointerErrorTexts = std::array {
        ErrorText<RootPointerError> { .error = RootPointerError::NotJson, .text = "its config is not JSON" },
        ErrorText<RootPointerError> { .error = RootPointerError::MissingKey,
                                      .text = "a key a root names is missing from its config" },
        ErrorText<RootPointerError> { .error = RootPointerError::NotAString,
                                      .text = "a root's directory in its config is not text" },
        ErrorText<RootPointerError> { .error = RootPointerError::StarOverNonList,
                                      .text = "a root walks a list its config does not hold" },
        ErrorText<RootPointerError> { .error = RootPointerError::BadPointer,
                                      .text = "its manifest names a root by an empty key" },
        ErrorText<RootPointerError> { .error = RootPointerError::NotAbsolute,
                                      .text = "a root's directory is not absolute (nor ~ or ~/...)" },
        ErrorText<RootPointerError> { .error = RootPointerError::EqualRoots,
                                      .text = "two of its roots are the same directory" },
        ErrorText<RootPointerError> {
            .error = RootPointerError::NoHome,
            .text = "a root's directory starts with ~, and the home directory is unknown" },
    };

    inline constexpr auto RootErrorTexts = std::array {
        ErrorText<RootError> { .error = RootError::EqualRoots,
                               .text = "two of its roots are the same directory" },
        ErrorText<RootError> { .error = RootError::Unresolved,
                               .text = "a command or argument names a root that is not resolved" },
    };

    inline constexpr auto StoreErrorTexts = std::array {
        ErrorText<StoreError> { .error = StoreError::Busy, .text = "another daemon holds it" },
        ErrorText<StoreError> { .error = StoreError::Io,
                                .text = "it cannot be created, made private or locked" },
    };

    /// @return The text @p texts holds for @p error; a generic one for an error it has no row for.
    template <typename Error, std::size_t N>
    [[nodiscard]] std::string_view textOf(std::array<ErrorText<Error>, N> const& texts, Error error) noexcept
    {
        auto const row = std::ranges::find(texts, error, &ErrorText<Error>::error);
        return row != texts.end() ? row->text : std::string_view { "an unexpected error" };
    }

    /// @return A problem of @p name, its message prefixed with where it is configured.
    [[nodiscard]] DeclarationProblem problemOf(std::string_view name,
                                               DaemonExtensionProblem problem,
                                               std::string_view what)
    {
        return DeclarationProblem { .name = std::string(name),
                                    .problem = problem,
                                    .message = std::format("daemon.extensions.{}: {}", name, what) };
    }

    /// Whether an extension waits for the user's answer.
    enum class ConsentWait : uint8_t
    {
        NotWaiting = 0, ///< It was not to start, or it was answered.
        Waiting,        ///< It was to start, and its grant is pending.
    };

    /// Hands a finished process's `procs` hint to the runner, while there is one: the processes are made
    /// before the runner, and outlive it.
    class HintRelay
    {
      public:
        /// Hints @p runner from now on; null hints nothing.
        void attach(DaemonExtensionRunner* runner) noexcept
        {
            auto const lock = std::scoped_lock { _mutex };
            _runner = runner;
        }

        /// Raises @p hint for the runner attached, if any.
        void hint(DaemonHint hint) noexcept
        {
            auto const lock = std::scoped_lock { _mutex };
            if (_runner != nullptr)
                _runner->hint(hint);
        }

      private:
        std::mutex _mutex;
        DaemonExtensionRunner* _runner = nullptr; ///< Guarded by _mutex.
    };
} // namespace

/// One daemon-scoped extension that composed: what it runs on, and its runner while it runs.
struct DaemonExtensionRuntime::Extension
{
    std::string name;
    std::string configJson;
    ExtensionAutostart autostart = ExtensionAutostart::OnDemand;
    std::string hostId;      ///< What the grants are keyed on: the configuration key and the module.
    std::string grantDigest; ///< What a grant must be recorded for: the manifest and the resolved roots.
    std::vector<ResolvedRoot> roots;
    ConsentQuestion question; ///< What the user is asked for its grant.
    ConsentWait consent = ConsentWait::NotWaiting;
    std::vector<std::byte> module; ///< Its bytes, until it is loaded: only a granted extension is.
    std::shared_ptr<Engine>
        engine; ///< The daemon world's, which @ref compiled was loaded on; null till then.
    std::shared_ptr<CompiledExtension> compiled; ///< Null until it runs.
    std::unique_ptr<FileExtensionStore> store;
    std::shared_ptr<HintRelay> relay = std::make_shared<HintRelay>();
    DeclaredProcessesConfig processesConfig; ///< What @ref processes was made from, to make them anew.
    std::unique_ptr<DeclaredProcesses> processes;
    std::unique_ptr<DaemonExtensionRunner> runner; ///< Null until it runs; last, as it reaches the rest.
};

DaemonExtensionRuntime::Composed DaemonExtensionRuntime::create(DaemonExtensionRuntimeConfig config,
                                                                DaemonExtensionRuntimeServices services)
{
    auto composed =
        Composed { .runtime = std::make_unique<DaemonExtensionRuntime>(Key {}, config, std::move(services)),
                   .problems = {} };
    auto& runtime = *composed.runtime;
    for (auto& declaration: config.declarations)
    {
        auto extension = runtime.compose(std::move(declaration), config);
        if (!extension)
        {
            composed.problems.push_back(std::move(extension.error()));
            continue;
        }
        runtime._extensions.push_back(std::move(*extension));
    }
    for (auto const& extension: runtime._extensions)
    {
        auto const keys = extension->store->keys("");
        auto const contents = keys && !keys->empty() ? StoreContents::HasKeys : StoreContents::Empty;
        if (bootStartOf(extension->autostart, contents) != BootStart::Yes)
            continue;
        if (auto ran = runtime.run(*extension); !ran)
            composed.problems.push_back(std::move(ran.error()));
    }
    return composed;
}

DaemonExtensionRuntime::DaemonExtensionRuntime(Key /*key*/,
                                               DaemonExtensionRuntimeConfig const& config,
                                               DaemonExtensionRuntimeServices services):
    _services { std::move(services) },
    _home { config.home },
    _limits { config.limits },
    _provider { _services.backend, World::Daemon, config.limits },
    _grants { _services.fileSystem, config.grantsFile },
    _pool { _services.backend, 1 },
    _lane { _pool.assignLane() }
{
}

DaemonExtensionRuntime::~DaemonExtensionRuntime()
{
    stop();
}

std::expected<std::unique_ptr<DaemonExtensionRuntime::Extension>, DeclarationProblem> DaemonExtensionRuntime::
    compose(DaemonExtensionDeclaration declaration, DaemonExtensionRuntimeConfig const& config)
{
    auto const& name = declaration.name;
    auto const lookup = ExtensionModuleLookup { .fileSystem = _services.fileSystem,
                                                .bundledDirectories = config.bundledDirectories };
    auto const path = findExtensionModule(name, declaration.modulePath, lookup)
                          .transform([&](std::filesystem::path const& found) {
                              return _services.fileSystem.weaklyCanonical(found);
                          });
    auto bytes = path.and_then([&](std::filesystem::path const& found) {
        return readExtensionModule(found, _services.fileSystem, _limits.maxModuleBytes);
    });
    if (!bytes)
        return std::unexpected(
            problemOf(name, DaemonExtensionProblem::ModuleUnreadable, bytes.error().reason));

    auto manifest = parseManifest(*bytes);
    if (!manifest && manifest.error() == ExtensionError::ManifestInvalid)
        return std::unexpected(
            problemOf(name, DaemonExtensionProblem::ManifestInvalid, "its module's manifest is not valid"));
    if (!manifest || manifest->placement != ManifestPlacement::Daemon)
        return std::unexpected(problemOf(
            name,
            DaemonExtensionProblem::NotADaemonModule,
            std::format("`{}` is a status-line extension: configure it under a profile's `extensions`",
                        name)));

    auto roots = resolveRoots(*manifest, declaration.configJson, _home, _services.fileSystem);
    if (!roots)
        return std::unexpected(problemOf(
            name, DaemonExtensionProblem::RootUnresolvable, textOf(RootPointerErrorTexts, roots.error())));

    if (declaration.storeDirectory.empty())
        return std::unexpected(problemOf(
            name, DaemonExtensionProblem::StoreUnavailable, "no store directory can be named for it"));
    auto store = FileExtensionStore::open(
        _services.fileSystem, _services.locker, declaration.storeDirectory, StoreLimits {});
    if (!store)
        return std::unexpected(problemOf(name,
                                         DaemonExtensionProblem::StoreUnavailable,
                                         std::format("its store {} cannot be opened: {}",
                                                     core::pathToUtf8(declaration.storeDirectory),
                                                     textOf(StoreErrorTexts, store.error()))));

    auto extension = std::make_unique<Extension>();
    extension->name = name;
    extension->configJson = std::move(declaration.configJson);
    extension->autostart = declaration.autostart;
    extension->hostId = extensionHostId(name, *path);
    extension->grantDigest = grantDigestOf(manifest->digest, *roots);
    extension->roots = *roots;
    extension->question =
        daemonConsentQuestion(DaemonConsentRequest { .hostId = extension->hostId,
                                                     .manifest = *manifest,
                                                     .roots = *roots,
                                                     .grantDigest = extension->grantDigest });
    extension->module = std::move(*bytes);
    extension->store = std::make_unique<FileExtensionStore>(std::move(*store));

    extension->processesConfig =
        DeclaredProcessesConfig { .manifest = std::move(*manifest),
                                  .roots = std::move(*roots),
                                  .inheritedEnvironment = config.inheritedEnvironment,
                                  .hostId = extension->hostId,
                                  .grantDigest = extension->grantDigest };
    if (auto made = makeProcesses(*extension); !made)
        return std::unexpected(
            problemOf(name, DaemonExtensionProblem::RootUnresolvable, textOf(RootErrorTexts, made.error())));
    return extension;
}

std::expected<void, RootError> DaemonExtensionRuntime::makeProcesses(Extension& extension)
{
    auto processes =
        DeclaredProcesses::create(extension.processesConfig,
                                  DeclaredProcessesServices { .runner = _services.runner,
                                                              .directories = _services.directories,
                                                              .grants = _grants,
                                                              .onFinished = [relay = extension.relay] {
                                                                  relay->hint(DaemonHint::Procs);
                                                              } });
    if (!processes)
        return std::unexpected(processes.error());
    extension.processes = std::move(*processes);
    return {};
}

std::expected<void, DeclarationProblem> DaemonExtensionRuntime::run(Extension& extension)
{
    if (extension.runner)
        return {};
    extension.consent = ConsentWait::NotWaiting;
    switch (_grants.lookup(extension.hostId, extension.grantDigest))
    {
        case GrantState::Granted: break;
        case GrantState::Denied:
            errorLog()("daemon.extensions.{}: the user denied it; it does not run.", extension.name);
            return {};
        case GrantState::Pending:
            errorLog()("daemon.extensions.{}: it waits for the user's consent in a Contour window.",
                       extension.name);
            extension.consent = ConsentWait::Waiting;
            return {};
    }
    if (auto loaded = load(extension); !loaded)
        return loaded;
    extension.runner =
        std::make_unique<DaemonExtensionRunner>(extension.engine,
                                                extension.compiled,
                                                DaemonRunnerConfig { .name = extension.name,
                                                                     .configJson = extension.configJson,
                                                                     .roots = extension.roots,
                                                                     .home = _home,
                                                                     .limits = _limits },
                                                DaemonServices { .processes = *extension.processes,
                                                                 .store = *extension.store,
                                                                 .presence = _services.presence,
                                                                 .scheduler = _pool,
                                                                 .wallClock = _services.wallClock,
                                                                 .localTime = _services.localTime,
                                                                 .retirement = _provider,
                                                                 .onLog = _services.onLog });
    extension.relay->attach(extension.runner.get());
    return {};
}

std::expected<void, DeclarationProblem> DaemonExtensionRuntime::load(Extension& extension)
{
    if (extension.compiled)
        return {};
    auto engine = _provider.acquire();
    if (!engine)
        return std::unexpected(problemOf(extension.name,
                                         DaemonExtensionProblem::RuntimeUnavailable,
                                         "the WebAssembly runtime is unavailable"));
    // A module is loaded on a thread prepared for the runtime, as every call into it is.
    auto const guard = _services.backend.enterThread();
    if (!guard)
        return std::unexpected(problemOf(extension.name,
                                         DaemonExtensionProblem::RuntimeUnavailable,
                                         "the WebAssembly runtime is unavailable on this thread"));
    auto compiled = CompiledExtension::load(**engine, extension.module);
    if (!compiled)
        return std::unexpected(
            problemOf(extension.name,
                      DaemonExtensionProblem::ModuleUnreadable,
                      std::format("its module does not load ({})", errorText(compiled.error()))));
    extension.module = {};
    extension.engine = std::move(*engine);
    extension.compiled = std::move(*compiled);
    return {};
}

void DaemonExtensionRuntime::runOrReport(Extension& extension)
{
    // The one place a granted extension that cannot run is reported, whether it was started or just granted.
    if (auto ran = run(extension); !ran)
        errorLog()("{}", ran.error().message);
}

DaemonExtensionRuntime::Extension* DaemonExtensionRuntime::find(std::string_view name) const
{
    auto const found = std::ranges::find(
        _extensions, name, [](auto const& extension) { return std::string_view { extension->name }; });
    return found != _extensions.end() ? found->get() : nullptr;
}

void DaemonExtensionRuntime::start(std::string_view name)
{
    if (_lifecycle == Lifecycle::Stopped)
        return;
    auto* const extension = find(name);
    if (extension == nullptr)
        return;
    if (extension->runner)
        extension->runner->restart();
    else
        runOrReport(*extension);
}

std::optional<DaemonRunnerStatus> DaemonExtensionRuntime::status(std::string_view name) const
{
    auto const* const extension = find(name);
    if (extension == nullptr || !extension->runner)
        return std::nullopt;
    return extension->runner->status();
}

std::vector<DaemonExtensionEntry> DaemonExtensionRuntime::entries() const
{
    auto listed = std::vector<DaemonExtensionEntry> {};
    if (_lifecycle == Lifecycle::Stopped)
        return listed;
    for (auto const& extension: _extensions)
    {
        auto phase = DaemonExtensionPhase::Stopped;
        if (extension->runner)
            phase = extension->runner->status().state == DaemonRunnerState::Disabled
                        ? DaemonExtensionPhase::Disabled
                        : DaemonExtensionPhase::Running;
        else
            switch (_grants.lookup(extension->hostId, extension->grantDigest))
            {
                case GrantState::Denied: phase = DaemonExtensionPhase::Denied; break;
                case GrantState::Pending:
                    if (extension->consent == ConsentWait::Waiting)
                        phase = DaemonExtensionPhase::Pending;
                    break;
                case GrantState::Granted: break;
            }
        listed.push_back(DaemonExtensionEntry {
            .name = extension->name, .phase = phase, .grantDigest = extension->grantDigest });
    }
    return listed;
}

std::vector<PendingConsent> DaemonExtensionRuntime::pending() const
{
    auto waiting = std::vector<PendingConsent> {};
    if (_lifecycle == Lifecycle::Stopped)
        return waiting;
    for (auto const& extension: _extensions)
        // Answered meanwhile through the grants file of a former run is answered all the same.
        if (extension->consent == ConsentWait::Waiting
            && _grants.lookup(extension->hostId, extension->grantDigest) == GrantState::Pending)
            waiting.push_back(PendingConsent { .name = extension->name,
                                               .grantDigest = extension->grantDigest,
                                               .question = extension->question });
    return waiting;
}

void DaemonExtensionRuntime::answer(std::string_view name, std::string_view grantDigest, GrantState answer)
{
    if (_lifecycle == Lifecycle::Stopped || answer == GrantState::Pending)
        return;
    auto* const extension = find(name);
    // Only the question still asked is answered: a reply may name any extension and digest.
    if (extension == nullptr || extension->consent != ConsentWait::Waiting
        || extension->grantDigest != grantDigest)
        return;
    _grants.record(extension->hostId, extension->grantDigest, answer);
    extension->consent = ConsentWait::NotWaiting;
    if (answer == GrantState::Granted)
        runOrReport(*extension);
}

void DaemonExtensionRuntime::revoke(std::string_view name)
{
    if (_lifecycle == Lifecycle::Stopped)
        return;
    auto* const extension = find(name);
    if (extension == nullptr)
        return;
    _grants.record(extension->hostId, extension->grantDigest, GrantState::Pending);
    if (!extension->runner)
        return; // Not running: forgotten, and asked again only when it is next started.
    extension->relay->attach(nullptr);
    extension->runner.reset();
    // The runner's last job, queued on the lane just now, still reaches its processes: they are cancelled,
    // and their results dropped, by the job queued behind it -- the pool has one worker, so its jobs run in
    // the order they were posted. Nothing they did reaches the instance of a later grant, which gets
    // processes of its own.
    _pool.post(_lane, [old = std::shared_ptr<DeclaredProcesses> { std::move(extension->processes) }] {});
    if (auto made = makeProcesses(*extension); !made)
        errorLog()("daemon.extensions.{}: its processes cannot be made anew; it does not run.",
                   extension->name);
    extension->consent = ConsentWait::Waiting;
}

void DaemonExtensionRuntime::stop() noexcept
{
    if (_lifecycle == Lifecycle::Stopped)
        return;
    _lifecycle = Lifecycle::Stopped;
    // The runners first: each posts its last job, which reaches its processes and store.
    for (auto const& extension: _extensions)
    {
        extension->relay->attach(nullptr);
        extension->runner.reset();
    }
    _pool.stop();
    // Then the processes, whose destruction cancels each one in flight and reaps its group.
    for (auto const& extension: _extensions)
        extension->processes.reset();
    _provider.releaseRetired();
}

} // namespace vtextension
