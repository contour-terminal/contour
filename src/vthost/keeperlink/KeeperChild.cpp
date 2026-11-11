// SPDX-License-Identifier: Apache-2.0
#include <vthost/keeperlink/KeeperChild.hpp>

#include <vtpty/ExecutablePath.hpp>
#include <vtpty/PreparedSpawn.hpp>
#include <vtpty/SpawnSpec.hpp>
#include <vtpty/UnixUtils.hpp>

#include <core/Assert.hpp>

#include <cerrno>
#include <expected>
#include <format>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>

#include <vthost/Logging.hpp>
#include <vthost/keeperlink/KeeperLink.hpp>

using namespace std::string_literals;

namespace vthost::keeperlink
{

namespace
{
    /// The most descriptors a keeper spawn carries: every trampoline slot and the spec socket, all but the
    /// process handle of what the keeper keeps, and the executable.
    constexpr auto MostSpawnDescriptors = std::size_t { vtpty::TrampolineSlotCount } + 1
                                          + (core::codec::EnumWire<keeper::KeptDescriptor>::Values.size() - 1)
                                          + 1;
    static_assert(MostSpawnDescriptors <= keeper::Spawn::MaxDescriptors,
                  "a keeper spawn must fit the descriptors a Spawn may carry");

    /// The trampoline, as the keeper is to run it.
    struct TrampolineExecutable
    {
        keeper::ExecutableSource source = keeper::ExecutableSource::Path;
        std::string path;          ///< For ExecutableSource::Path.
        vtpty::OwnedFd descriptor; ///< For ExecutableSource::Descriptor.
    };

    /// @return This process's own image, which is the trampoline: on Linux by descriptor, so that the keeper
    ///         runs the very image this daemon runs, whatever replaced the file since; elsewhere by the path
    ///         of the staged, immutable image it runs from.
    [[nodiscard]] std::expected<TrampolineExecutable, std::error_code> trampolineExecutable()
    {
#ifdef __linux__
        auto descriptor = vtpty::OwnedFd { ::open("/proc/self/exe", O_PATH | O_CLOEXEC) };
        if (!descriptor.isOpen())
            return std::unexpected(std::error_code { errno, std::generic_category() });
        return TrampolineExecutable { .source = keeper::ExecutableSource::Descriptor,
                                      .path = {},
                                      .descriptor = std::move(descriptor) };
#else
        return vtpty::selfExecutablePath().transform([](std::string path) {
            return TrampolineExecutable { .source = keeper::ExecutableSource::Path,
                                          .path = std::move(path),
                                          .descriptor = {} };
        });
#endif
    }

    /// @return Why the keeper could not spawn, as a sentence fragment.
    [[nodiscard]] std::string_view describe(keeper::SpawnError error) noexcept
    {
        switch (error)
        {
            case keeper::SpawnError::Unknown: return "the keeper did not answer, or refused the request";
            case keeper::SpawnError::ForkFailed: return "the keeper could not fork";
            case keeper::SpawnError::ExecutableUnusable:
                return "the keeper could not run the spawn trampoline";
            case keeper::SpawnError::TooManyDescriptors: return "the keeper is out of descriptors";
            case keeper::SpawnError::ShuttingDown:
                return "the keeper takes on nothing new as the daemon ends";
        }
        return "the keeper could not spawn";
    }

    [[nodiscard]] std::unexpected<vtpty::StartFailure> spawnFailure(std::string detail)
    {
        return std::unexpected(
            vtpty::StartFailure { .error = vtpty::StartError::SpawnFailed, .detail = std::move(detail) });
    }
} // namespace

// {{{ ChildExit, KeeperExits

void ChildExit::record(vtpty::ExitStatus status)
{
    auto const _ = std::scoped_lock { _lock };
    if (!_status)
        _status = status;
}

std::optional<vtpty::ExitStatus> ChildExit::status() const
{
    auto const _ = std::scoped_lock { _lock };
    return _status;
}

std::shared_ptr<ChildExit> KeeperExits::watch(keeper::KeeperSessionId session)
{
    auto exit = std::make_shared<ChildExit>();
    auto const _ = std::scoped_lock { _lock };
    std::erase_if(_watched, [](auto const& entry) { return entry.second.expired(); });
    _watched.insert_or_assign(session, exit);
    return exit;
}

void KeeperExits::exited(keeper::KeeperSessionId session, vtpty::ExitStatus status)
{
    auto const _ = std::scoped_lock { _lock };
    auto const found = _watched.find(session);
    if (found == _watched.end())
        return;
    if (auto const exit = found->second.lock())
        exit->record(status);
    _watched.erase(found);
}

// }}}
// {{{ KeeperChild

KeeperChild::KeeperChild(KeeperLink& link,
                         std::shared_ptr<KeeperExits> exits,
                         KeeperChildConfig config,
                         std::unique_ptr<vtpty::Pty> pty):
    _link { link }, _exits { std::move(exits) }, _config { std::move(config) }, _pty { std::move(pty) }
{
    Require(_exits != nullptr);
    Require(_config.placement != nullptr);
    Require(_pty != nullptr);
}

KeeperChild::~KeeperChild()
{
    close();
}

std::optional<keeper::KeeperSessionId> KeeperChild::session() const noexcept
{
    if (_session == 0)
        return std::nullopt;
    return keeper::KeeperSessionId { _session };
}

vtpty::StartResult KeeperChild::start()
{
    if (_session != 0)
        return {};
    if (auto started = _pty->start(); !started)
        return started;
    return spawn();
}

vtpty::StartResult KeeperChild::spawn()
{
    auto prepared = vtpty::prepareTrampolineSpawn(
        *_pty, _config.exec, _config.sandbox, _config.placement->childOomScoreAdjust());
    if (!prepared)
        return std::unexpected(prepared.error());
    auto const terminal = _pty->handover();
    if (!terminal)
        return spawnFailure("The terminal is not a Unix pseudo terminal.");
    auto executable = trampolineExecutable();
    if (!executable)
        return spawnFailure(
            std::format("The spawn trampoline cannot be named: {}", executable.error().message()));

    // The child's descriptors, then what the keeper keeps of them, then the executable.
    auto request = keeper::Spawn { .request = 0,
                                   .argv = prepared->argv,
                                   .executablePath = executable->path,
                                   .childFds = {},
                                   .kept = {},
                                   .executable = executable->source };
    auto fds = std::vector<int> {};
    for (auto const& [target, copy]: prepared->childFds)
    {
        request.childFds.push_back(static_cast<uint32_t>(target));
        fds.push_back(copy.get());
    }
    auto const keep = [&](keeper::KeptDescriptor role, int fd) {
        if (fd < 0)
            return;
        request.kept.push_back(role);
        fds.push_back(fd);
    };
    keep(keeper::KeptDescriptor::Master, terminal->masterFd);
    // A writer on the fast pipe must not meet EPIPE when the daemon dies.
    if (terminal->stdoutFastPipe)
        keep(keeper::KeptDescriptor::FastPipeReader, terminal->stdoutFastPipe->first);
    keep(keeper::KeptDescriptor::Gate, prepared->gate.get());
    keep(keeper::KeptDescriptor::Settle, prepared->settle.get());
    if (executable->descriptor.isOpen())
        fds.push_back(executable->descriptor.get());

    auto reply = _link.spawn(std::move(request), std::move(fds));
    // The child holds its own now, or never will.
    prepared->childFds.clear();
    _pty->closeSpawnDescriptors();
    if (!reply)
        return spawnFailure(std::format("The shell could not be started: {}.", describe(reply.error())));

    _session = reply->spawned.session;
    _pid = reply->spawned.pid;
    _master = terminal->masterFd;
    _exit = _exits->watch(keeper::KeeperSessionId { _session });

    // Written once the trampoline exists: a spec larger than the socket's buffer is read as it is written.
    if (auto written = vtpty::writeSpawnSpec(*prepared); !written)
        return spawnFailure(
            std::format("The shell's spawn spec could not be handed over: {}", written.error().message()));
    {
        auto const _ = std::scoped_lock { _settleLock };
        _settle = std::move(prepared->settle);
    }
    // Placed exactly as a child of the daemon's own is; then the keeper lets go of the gate.
    _config.placement->placeThenRelease(
        vtpty::ParkedChild { static_cast<int>(_pid),
                             vtpty::OwnedFd { reply->processHandle.release() },
                             std::move(prepared->gate),
                             vtpty::ProcessRole::Session });
    _link.post(keeper::Placed { .session = _session });
    return {};
}

void KeeperChild::close()
{
    _pty->close();
    if (_session == 0)
        return;
    // The keeper closes its own master and kills the child after the grace, if it is still there.
    std::call_once(_released, [this] {
        _link.post(keeper::Release { .session = _session,
                                     .graceMs = static_cast<uint32_t>(_config.hangupGrace.count()) });
    });
}

vtpty::SpawnProgress KeeperChild::spawnProgress() const
{
    auto const _ = std::scoped_lock { _settleLock };
    return vtpty::settleProgressOf(_settle);
}

std::optional<vtpty::ExitStatus> KeeperChild::checkStatus() const
{
    return _exit ? _exit->status() : std::nullopt;
}

std::string KeeperChild::workingDirectory() const
{
    // Once it ended, its pid may name another process.
    if (_pid <= 0 || checkStatus())
        return "."s;
    // Until its trampoline changed into it and settled, the child is still in the keeper's directory, which
    // it is about to leave: the one it starts in is the answer (as Process::workingDirectory() has it).
    if (!_config.exec.workingDirectory.empty() && spawnProgress() == vtpty::SpawnProgress::InProgress)
        return _config.exec.workingDirectory.string();
    return vtpty::util::workingDirectoryOf(static_cast<pid_t>(_pid)).value_or("."s);
}

std::optional<std::string> KeeperChild::foregroundProcessName() const
{
    if (_session == 0 || _pty->isClosed())
        return std::nullopt;
    return vtpty::util::foregroundProcessNameOf(_master);
}

void KeeperChild::terminate(vtpty::TerminationHint hint)
{
    if (_session == 0 || checkStatus())
        return;
    switch (hint)
    {
        case vtpty::TerminationHint::Normal:
            _link.post(keeper::Signal { .session = _session, .signal = keeper::ChildSignal::Terminate });
            break;
        case vtpty::TerminationHint::Kill:
            _link.post(keeper::Signal { .session = _session, .signal = keeper::ChildSignal::Kill });
            break;
        case vtpty::TerminationHint::Hangup: close(); break;
    }
}

vtpty::ChildCrossing KeeperChild::crossing() const
{
    return vtpty::ChildCrossing { .kind = vtpty::CrossingKind::Keeper,
                                  .child = std::nullopt,
                                  .keeperSession = _session };
}

// }}}

} // namespace vthost::keeperlink
