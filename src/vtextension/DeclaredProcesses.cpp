// SPDX-License-Identifier: Apache-2.0
#include <vtextension/DeclaredProcesses.hpp>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <vtextension/ArgumentKind.hpp>
#include <vtextension/ChildEnvironment.hpp>
#include <vtextension/DeclaredArguments.hpp>
#include <vtextension/RootResolution.hpp>
#include <vtextension/SelfPrograms.hpp>

namespace vtextension
{

namespace
{
    /// How a vtpty completion reaches the guest, indexed by vtpty::ProcessCompletion. A cancelled process,
    /// and one whose status was lost, was ended by a signal as far as the guest can tell.
    constexpr auto CompletionOf = std::array {
        ProcCompletion::Exited,    // Exited
        ProcCompletion::Signalled, // Signalled
        ProcCompletion::TimedOut,  // TimedOut
        ProcCompletion::Signalled, // Cancelled
        ProcCompletion::Signalled, // StatusLost
    };
    static_assert(CompletionOf.size() == std::to_underlying(vtpty::ProcessCompletion::StatusLost) + 1U);

    /// Why a start the runner refused is refused, indexed by vtpty::ProcessStartError. Only a missing
    /// directory is CwdMissing: a missing program is a refused start, never a vanished worktree.
    constexpr auto StartErrorOf = std::array {
        ProcessError::CwdMissing,  // DirectoryMissing
        ProcessError::SpawnFailed, // ProgramNotFound
        ProcessError::SpawnFailed, // SpawnFailed
        ProcessError::Unsupported, // Unsupported
    };
    static_assert(StartErrorOf.size() == std::to_underlying(vtpty::ProcessStartError::Unsupported) + 1U);

    /// Why a working directory is refused, indexed by CwdRefusal: the words agent panes use too.
    constexpr auto CwdErrorOfRefusal = std::array {
        ProcessError::CwdOutsideRoots,  // OutsideRoot
        ProcessError::CwdMissing,       // Missing
        ProcessError::CwdNotADirectory, // NotADirectory
        ProcessError::Unsupported,      // Unsupported
    };
    static_assert(CwdErrorOfRefusal.size() == std::to_underlying(CwdRefusal::Unsupported) + 1U);

    /// @return Why a working directory refused for @p error is refused, by cwdRefusalOf().
    [[nodiscard]] ProcessError cwdErrorOf(RootError error) noexcept
    {
        return CwdErrorOfRefusal.at(std::to_underlying(cwdRefusalOf(error)));
    }

    [[nodiscard]] OutputExtent extentOf(vtpty::OutputExtent extent) noexcept
    {
        return extent == vtpty::OutputExtent::Whole ? OutputExtent::Whole : OutputExtent::Truncated;
    }

    /// Keeps at most @p cap bytes of @p stream.
    /// @return Whether what is kept is all the process wrote.
    [[nodiscard]] OutputExtent keep(vtpty::CapturedStream& stream, std::size_t cap)
    {
        if (stream.bytes.size() <= cap)
            return extentOf(stream.extent);
        stream.bytes.resize(cap);
        return OutputExtent::Truncated;
    }
} // namespace

struct DeclaredProcesses::State
{
    /// A finished process's result and output.
    struct Finished
    {
        ProcessResult result;
        std::string standardOutput;
        std::string standardError;
        uint64_t order = 0; ///< When it finished, among this extension's processes; the oldest go first.

        [[nodiscard]] std::size_t bytes() const noexcept
        {
            return standardOutput.size() + standardError.size();
        }
    };

    /// A process started and not released.
    struct Entry
    {
        std::string tag;
        std::string command;
        std::optional<vtpty::ProcessToken> token; ///< Set once the runner has started it, until it finishes.
        std::optional<Finished> finished;
    };

    State(ProcessLimits limits, std::function<void()> onFinished):
        limits { limits }, onFinished { std::move(onFinished) }
    {
    }

    /// With the mutex held: whether another process may start now.
    [[nodiscard]] bool limitReached() const noexcept
    {
        auto const finished = static_cast<std::size_t>(std::ranges::count_if(
            entries, [](auto const& entry) { return entry.second.finished.has_value(); }));
        return entries.size() - finished >= limits.maxRunning || finished >= limits.maxUntaken;
    }

    /// Records how a process ended, on the runner's thread, and posts the hint.
    void finish(ProcessHandle handle, vtpty::ProcessExit exit)
    {
        {
            auto const lock = std::scoped_lock { mutex };
            auto const entry = entries.find(handle);
            if (lifecycle == Lifecycle::Closed || entry == entries.end())
                return; // Released, or the service destroyed, while it ran.
            auto const stdoutExtent = keep(exit.standardOutput, limits.streamCap);
            auto const stderrExtent = keep(
                exit.standardError,
                std::min(limits.streamCap,
                         limits.processCap - std::min(limits.processCap, exit.standardOutput.bytes.size())));
            auto finished = Finished {
                .result = ProcessResult { .completion = CompletionOf.at(std::to_underlying(exit.completion)),
                                          .exitCode = exit.exitCode,
                                          .stdoutBytes = exit.standardOutput.bytes.size(),
                                          .stdoutExtent = stdoutExtent,
                                          .stderrBytes = exit.standardError.bytes.size(),
                                          .stderrExtent = stderrExtent },
                .standardOutput = std::move(exit.standardOutput.bytes),
                .standardError = std::move(exit.standardError.bytes),
                .order = ++finishCount,
            };
            keptBytes += finished.bytes();
            entry->second.token.reset();
            entry->second.finished = std::move(finished);
            releaseOldestPastTotal();
            if (!onFinished)
                return;
            ++hintsRunning;
        }
        onFinished();
        auto const lock = std::scoped_lock { mutex };
        --hintsRunning;
        hintsDone.notify_all();
    }

    /// With the mutex held: releases the oldest finished results while more than the total is kept.
    void releaseOldestPastTotal()
    {
        while (keptBytes > limits.totalCap)
        {
            auto finished = entries | std::views::filter([](auto const& entry) {
                                return entry.second.finished.has_value();
                            });
            auto const oldest = std::ranges::min_element(
                finished, {}, [](auto const& entry) { return entry.second.finished->order; });
            if (oldest == finished.end())
                return;
            keptBytes -= oldest->second.finished->bytes();
            entries.erase(oldest.base());
        }
    }

    /// Whether completions still reach the service.
    enum class Lifecycle : uint8_t
    {
        Open,   ///< They are recorded, and post the hint.
        Closed, ///< The service is being destroyed: they are dropped.
    };

    ProcessLimits limits;
    std::function<void()> onFinished;
    mutable std::mutex mutex;
    std::condition_variable hintsDone; ///< Notified whenever an onFinished call returns.
    Lifecycle lifecycle = Lifecycle::Open;
    std::size_t hintsRunning = 0; ///< How many onFinished calls are running now.
    std::map<ProcessHandle, Entry> entries;
    uint32_t lastHandle = 0;
    uint64_t finishCount = 0;
    std::size_t keptBytes = 0; ///< The bytes of every finished result in entries.
};

std::expected<std::unique_ptr<DeclaredProcesses>, RootError> DeclaredProcesses::create(
    DeclaredProcessesConfig config, DeclaredProcessesServices services)
{
    if (auto const distinct = checkRootsDistinct(config.roots, services.directories.pathStyle()); !distinct)
        return std::unexpected(distinct.error());
    auto const resolved = [&](std::string_view label) {
        return std::ranges::contains(config.roots, label, &ResolvedRoot::label);
    };
    auto const commandRootsResolved = std::ranges::all_of(
        config.manifest.commands, [&](DeclaredCommand const& command) { return resolved(command.root); });
    auto const argumentRootsResolved =
        std::ranges::all_of(config.manifest.arguments, [&](ArgumentDecl const& argument) {
            return argument.kind != ArgumentKind::Path || resolved(argument.root);
        });
    if (!commandRootsResolved || !argumentRootsResolved)
        return std::unexpected(RootError::Unresolved);
    return std::make_unique<DeclaredProcesses>(Key {}, std::move(config), std::move(services));
}

DeclaredProcesses::DeclaredProcesses(Key /*key*/,
                                     DeclaredProcessesConfig config,
                                     DeclaredProcessesServices services):
    _config { std::move(config) },
    _runner { services.runner },
    _directories { services.directories },
    _grants { services.grants },
    _state { std::make_shared<State>(_config.limits, std::move(services.onFinished)) }
{
}

DeclaredProcesses::~DeclaredProcesses()
{
    auto running = std::vector<vtpty::ProcessToken> {};
    {
        auto const lock = std::scoped_lock { _state->mutex };
        _state->lifecycle = State::Lifecycle::Closed;
        for (auto const& entry: _state->entries)
            if (entry.second.token)
                running.push_back(*entry.second.token);
        _state->entries.clear();
    }
    for (auto const token: running)
        _runner.cancel(token);
    auto lock = std::unique_lock { _state->mutex };
    _state->hintsDone.wait(lock, [this] { return _state->hintsRunning == 0; });
}

std::expected<void, RootError> DeclaredProcesses::ensureDeclaredRoot(std::string_view root) const
{
    auto const declared = std::ranges::find(_config.manifest.roots, root, &RootDecl::label);
    if (declared == _config.manifest.roots.end() || declared->creation != RootCreation::CreateIfMissing)
        return {};
    return ensureRoot(_directories, _config.roots, root);
}

std::expected<void, RootError> DeclaredProcesses::makePathParents(DeclaredCommand const& command,
                                                                  std::vector<std::string>& argv) const
{
    for (auto const index: std::views::iota(std::size_t { 0 }, argv.size()))
    {
        auto const name = placeholderName(command.argvTemplate.at(index));
        if (!name)
            continue;
        auto const declared = std::ranges::find(_config.manifest.arguments, *name, &ArgumentDecl::name);
        if (declared == _config.manifest.arguments.end() || declared->kind != ArgumentKind::Path)
            continue;
        auto made = ensureDeclaredRoot(declared->root).and_then([&] {
            return createPathParents(_directories, _config.roots, declared->root, argv.at(index));
        });
        if (!made)
            return std::unexpected(made.error());
        argv.at(index) = std::move(*made);
    }
    return {};
}

std::expected<ProcessHandle, ProcessError> DeclaredProcesses::start(std::string_view command,
                                                                    ConfigMap const& arguments,
                                                                    std::string_view cwd,
                                                                    std::string_view tag)
{
    auto const declared = std::ranges::find(_config.manifest.commands, command, &DeclaredCommand::name);
    if (declared == _config.manifest.commands.end())
        return std::unexpected(ProcessError::NotDeclared);

    switch (_grants.lookup(_config.hostId, _config.grantDigest))
    {
        case GrantState::Pending: return std::unexpected(ProcessError::Pending);
        case GrantState::Denied: return std::unexpected(ProcessError::Denied);
        case GrantState::Granted: break;
    }

    auto argv =
        substituteDeclared(declared->argvTemplate, _config.manifest, arguments, _directories, _config.roots);
    if (!argv)
        return std::unexpected(ProcessError::ArgumentRejected);

    {
        auto const lock = std::scoped_lock { _state->mutex };
        if (_state->limitReached())
            return std::unexpected(ProcessError::TooMany);
    }

    if (auto const ensured = ensureDeclaredRoot(declared->root); !ensured)
        return std::unexpected(cwdErrorOf(ensured.error()));

    auto directory = openInRoot(_directories, _config.roots, declared->root, cwd);
    if (!directory)
        return std::unexpected(cwdErrorOf(directory.error()));

    // Reserved before anything is created and the runner starts it, so that its completion, which may come at
    // once on the runner's thread, finds it; checked again, so that two concurrent starts cannot both take
    // the last place.
    auto handle = ProcessHandle {};
    {
        auto const lock = std::scoped_lock { _state->mutex };
        if (_state->limitReached())
            return std::unexpected(ProcessError::TooMany);
        handle = ProcessHandle { ++_state->lastHandle };
        _state->entries.emplace(handle,
                                State::Entry { .tag = std::string(tag),
                                               .command = declared->name,
                                               .token = std::nullopt,
                                               .finished = std::nullopt });
    }

    // Directories are created only now, once nothing else refuses the start.
    if (auto const made = makePathParents(*declared, *argv); !made)
    {
        auto const lock = std::scoped_lock { _state->mutex };
        _state->entries.erase(handle);
        return std::unexpected(cwdErrorOf(made.error()));
    }

    resolveProgram(*argv, _config.programs);
    auto spec = vtpty::ProcessSpec {
        .argv = std::move(*argv),
        .workingDirectory = std::move(*directory),
        .environment = childEnvironment(_config.inheritedEnvironment,
                                        _config.manifest.commandEnvironment,
                                        declared->environment,
                                        NativeEnvironmentNameCase),
        .timeout = declared->timeout == CommandTimeout::Short ? _config.timeouts.shortClass
                                                              : _config.timeouts.longClass,
        .outputCap = _config.limits.streamCap,
        .standardOutput = vtpty::StreamCapture::Capture,
        .standardError = vtpty::StreamCapture::Capture,
    };

    auto const started = _runner.start(
        std::move(spec),
        [state = std::weak_ptr<State>(_state), handle](vtpty::ProcessToken, vtpty::ProcessExit exit) {
            if (auto const alive = state.lock())
                alive->finish(handle, std::move(exit));
        });

    auto lock = std::unique_lock { _state->mutex };
    auto const entry = _state->entries.find(handle);
    if (!started)
    {
        if (entry != _state->entries.end())
            _state->entries.erase(entry);
        return std::unexpected(StartErrorOf.at(std::to_underlying(started.error())));
    }
    if (entry == _state->entries.end())
    {
        // Released while it was being started: stop it.
        lock.unlock();
        _runner.cancel(*started);
        return handle;
    }
    if (!entry->second.finished)
        entry->second.token = *started;
    return handle;
}

std::vector<ProcessInfo> DeclaredProcesses::list() const
{
    auto const lock = std::scoped_lock { _state->mutex };
    auto infos = std::vector<ProcessInfo> {};
    infos.reserve(_state->entries.size());
    for (auto const& [handle, entry]: _state->entries)
        infos.push_back(ProcessInfo {
            .handle = handle,
            .tag = entry.tag,
            .command = entry.command,
            .result = entry.finished ? std::optional { entry.finished->result } : std::nullopt,
        });
    return infos;
}

std::expected<OutputPage, ProcessError> DeclaredProcesses::take(ProcessHandle handle,
                                                                ProcStream stream,
                                                                std::size_t offset) const
{
    auto const lock = std::scoped_lock { _state->mutex };
    auto const entry = _state->entries.find(handle);
    if (entry == _state->entries.end())
        return std::unexpected(ProcessError::UnknownHandle);
    if (!entry->second.finished)
        return std::unexpected(ProcessError::StillRunning);
    auto const& finished = *entry->second.finished;
    auto const bytes =
        std::string_view(stream == ProcStream::Stdout ? finished.standardOutput : finished.standardError);
    if (offset >= bytes.size())
        return OutputPage {};
    auto const page = bytes.substr(offset, _config.limits.pageBytes);
    return OutputPage { .bytes = std::string(page), .remaining = bytes.size() - offset - page.size() };
}

std::expected<void, ProcessError> DeclaredProcesses::release(ProcessHandle handle)
{
    auto running = std::optional<vtpty::ProcessToken> {};
    {
        auto const lock = std::scoped_lock { _state->mutex };
        auto const entry = _state->entries.find(handle);
        if (entry == _state->entries.end())
            return std::unexpected(ProcessError::UnknownHandle);
        if (entry->second.finished)
            _state->keptBytes -= entry->second.finished->bytes();
        running = entry->second.token;
        _state->entries.erase(entry);
    }
    if (running)
        _runner.cancel(*running);
    return {};
}

} // namespace vtextension
