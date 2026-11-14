// SPDX-License-Identifier: Apache-2.0
#include <vtpty/CommandLine.hpp>
#include <vtpty/DetachedProcess.hpp>
#include <vtpty/EnvironmentBlock.hpp>
#include <vtpty/PreparedSpawn.hpp>
#include <vtpty/Process.hpp>
#include <vtpty/ProcessPlacement.hpp>
#include <vtpty/Pty.hpp>
#include <vtpty/SandboxInfo.hpp>
#include <vtpty/SpawnSpec.hpp>
#include <vtpty/SpawnTrampoline.hpp>
#include <vtpty/UnixPty.hpp>
#include <vtpty/UnixUtils.hpp>

#include <core/Assert.hpp>
#include <core/Environment.hpp>
#include <core/Overloaded.hpp>
#include <core/UserInfo.hpp>
#include <core/Utils.hpp>
#include <core/platform/NativeFileSystem.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>

#ifndef __FreeBSD__
    #include <utmp.h>
#endif

#ifdef __APPLE__
    #include <libproc.h>
    #include <util.h>
#endif

#ifdef __linux__
    #include <sys/syscall.h>

    #include <pty.h>
#endif

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <csignal>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

using namespace std;
using namespace std::string_view_literals;
using core::trimRight;

namespace fs = std::filesystem;

namespace vtpty
{

namespace
{
    constexpr auto StdoutFastPipeEnvironmentName = "STDOUT_FASTPIPE"sv;

    /// The id a Process holds while it has no child. @see Process::Private::pid.
    constexpr pid_t NoChild = -1;

    /// How often a HangupGrace looks whether the child has ended, and whether the grace has passed.
    constexpr auto HangupGraceStep = std::chrono::milliseconds { 10 };

    /// @return The signal that asks a child to end as @p hint says.
    [[nodiscard]] constexpr int signalFor(Process::TerminationHint hint) noexcept
    {
        switch (hint)
        {
            case Process::TerminationHint::Normal: return SIGTERM;
            case Process::TerminationHint::Hangup: return SIGHUP;
            case Process::TerminationHint::Kill: return SIGKILL;
        }
        return SIGTERM;
    }

} // anonymous namespace

struct Process::Private
{
    string path;
    vector<string> args;
    fs::path cwd;
    Environment env;
    vector<string> removedEnvironment;
    bool escapeSandbox;

    unique_ptr<Pty> pty {};
    shared_ptr<ProcessPlacement> placement {};
    HangupGrace hangupGrace;

    /// Whether this process spawned the child itself.
    enum class Origin : uint8_t
    {
        Spawned = 0, ///< By start().
        Adopted,     ///< Left behind by a previous image; running already.
    };
    Origin origin = Origin::Spawned;

    // The child's id, or NoChild: before start() spawned one, when the spawn failed, and once the child has
    // been reaped. Never 0, which waitpid() and kill() read as the caller's whole process group, nor any
    // other id they would read as more than one process. Written under exitStatusMutex.
    mutable pid_t pid = NoChild;
    mutable std::mutex exitStatusMutex {};
    mutable std::optional<Process::ExitStatus> exitStatus {};

    /// The parent's end of the settle signal: a socket pair the child writes one byte to, then closes, once
    /// its terminal is set up, so that this end turns readable (the byte, or end-of-file as a child exits
    /// before it got there). Open from the spawn until observed readable. A helper another thread forks in
    /// between may hold the child's end on, which the byte makes harmless.
    /// @see Process::spawnProgress()
    mutable OwnedFd settleSignal {};
    mutable std::mutex settleSignalMutex {}; ///< Guards settleSignal.

    /// Started by close() while the child runs: kills it once hangupGrace has passed without it ending.
    std::optional<std::thread> hangupWatch {};
    std::mutex hangupWatchMutex {}; ///< Guards hangupWatch.

    [[nodiscard]] std::optional<ExitStatus> checkStatus(bool waitForExit) const;

    /// @return The child as an adopter takes it on, or nullopt where there is none. Under exitStatusMutex.
    [[nodiscard]] std::optional<AdoptedChild> adoptedChildLocked() const
    {
        if (pid == NoChild)
            return nullopt;
        return AdoptedChild {
            .pid = pid, .executable = path, .args = args, .cwd = cwd, .hangupGrace = hangupGrace
        };
    }

    /// @return The child's id, or NoChild when there is none to ask about, wait for or signal.
    [[nodiscard]] pid_t child() const
    {
        auto const _ = lock_guard { exitStatusMutex };
        return pid;
    }
};

Process::Process(string const& path,
                 vector<string> const& args,
                 fs::path const& cwd,
                 Environment const& env,
                 vector<string> removedEnvironment,
                 bool escapeSandbox,
                 unique_ptr<Pty> pty,
                 shared_ptr<ProcessPlacement> placement,
                 HangupGrace hangupGrace):
    _d(new Private { .path = path,
                     .args = args,
                     .cwd = cwd,
                     .env = env,
                     .removedEnvironment = std::move(removedEnvironment),
                     .escapeSandbox = escapeSandbox,
                     .pty = std::move(pty),
                     .placement = std::move(placement),
                     .hangupGrace = hangupGrace },
       [](Private* p) { delete p; })
{
    Require(_d->placement != nullptr);
}

bool Process::isFlatpak()
{
    // The narrow half of what currentSandbox() reads, rather than a second look at the same file:
    // one reader, one memo, one answer. @see SandboxInfo.hpp.
    return currentSandbox().state == SandboxState::Flatpak;
}

ProgramHost Process::programHost(SandboxEscape escape)
{
    return programHostFor(currentSandbox().state, escape);
}

SpawnProgress settleProgressOf(OwnedFd& settle) noexcept
{
    if (!settle.isOpen())
        return SpawnProgress::Settled;
    auto pollFd = pollfd { .fd = settle.get(), .events = POLLIN, .revents = 0 };
    auto ready = 0;
    while ((ready = ::poll(&pollFd, 1, 0)) < 0 && errno == EINTR)
        ;
    if (ready == 0)
        return SpawnProgress::InProgress;
    settle.reset();
    return SpawnProgress::Settled;
}

std::expected<PreparedSpawn, StartFailure> prepareTrampolineSpawn(Pty& startedPty,
                                                                  Process::ExecInfo const& exec,
                                                                  SandboxEscape escape,
                                                                  std::optional<int> childOomScoreAdjust)
{
    auto const spawnFailure = [](std::string detail) {
        return std::unexpected(
            StartFailure { .error = StartError::SpawnFailed, .detail = std::move(detail) });
    };
    auto const descriptors = startedPty.spawnDescriptors();
    if (!descriptors)
        return spawnFailure("The terminal is not a Unix pseudo terminal.");
    auto const hasStdoutFastPipe = descriptors->stdoutFastPipeWriter.has_value();

    // When escaping the sandbox, flatpak-spawn is handed the variables as --env= arguments and the
    // child keeps our environment untouched.
    auto const host = Process::programHost(escape);
    auto const passesOwnEnvironment = host == ProgramHost::ThisMachine;

    // The environment is assembled here, so that the trampoline can install it as it is -- and always, so
    // that a trampoline another process forks (the PTY keeper) runs the program under this one's.
    auto childEnvironment = [&]() -> vector<string> {
        auto overrides = Process::Environment {};
        auto removed = vector<string> {};
        if (passesOwnEnvironment)
        {
            if (Process::isFlatpak())
                overrides["TERMINFO"] = "/app/share/terminfo";
            for (auto const& [name, value]: exec.env)
                overrides[name] = value;
            if (hasStdoutFastPipe)
                overrides[string { StdoutFastPipeEnvironmentName }] = std::to_string(StdoutFastPipeFd);
            removed = exec.removedEnvironment;
        }
        return buildEnvironmentBlock(
            util::environmentEntries(), overrides, removed, EnvironmentNames::CaseSensitive);
    }();

    // What runs: the program as configured -- or, where it names no program but holds whitespace
    // ("bash -l"), the command line it spells -- looked up on the PATH the child execs with, a relative
    // path in the directory the child changes to before it execs; looked up nowhere when flatpak-spawn
    // runs it on the host (programsFor()).
    auto const searchPath =
        passesOwnEnvironment
            ? string { environmentValue(childEnvironment, "PATH", EnvironmentNames::CaseSensitive)
                           .value_or(string_view {}) }
            : string {};
    auto const invocation = invocationToRun(
        exec.program,
        exec.arguments,
        programsFor(host,
                    ProgramSearch { .path = searchPath,
                                    .workingDirectory = exec.workingDirectory,
                                    .fileSystem = core::platform::NativeFileSystem::instance() }));

    auto childArgv = [&]() -> vector<string> {
        if (host == ProgramHost::ThisMachine)
        {
            auto args = vector<string> { invocation.program };
            args.insert(args.end(), invocation.arguments.begin(), invocation.arguments.end());
            return args;
        }

        auto const terminfoBaseDirectory =
            Process::homeDirectory() / ".var/app/org.contourterminal.Contour/terminfo";

        // Prepend flatpak to jump out of sandbox:
        // flatpak-spawn --host --watch-bus --env=TERM=$TERM /bin/zsh
        auto realArgs = vector<string> { "/usr/bin/flatpak-spawn" };
        realArgs.emplace_back("--host");
        realArgs.emplace_back("--watch-bus");
        realArgs.emplace_back(std::format("--env=TERMINFO={}", terminfoBaseDirectory.generic_string()));
        if (hasStdoutFastPipe)
        {
            realArgs.emplace_back(std::format("--forward-fd={}", StdoutFastPipeFd));
            realArgs.emplace_back(
                std::format("--env={}={}", StdoutFastPipeEnvironmentName, StdoutFastPipeFd));
        }
        if (!exec.workingDirectory.empty())
            realArgs.emplace_back(std::format("--directory={}", exec.workingDirectory.generic_string()));
        realArgs.emplace_back(std::format("--env=TERM={}", "contour"));
        for (auto&& [name, value]: exec.env)
            realArgs.emplace_back(std::format("--env={}={}", name, value));
        if (hasStdoutFastPipe)
            realArgs.emplace_back(
                std::format("--env={}={}", StdoutFastPipeEnvironmentName, StdoutFastPipeFd));
        realArgs.push_back(invocation.program);
        for (auto const& arg: invocation.arguments)
            realArgs.push_back(arg);

        return realArgs;
    }();

    // The same kind of channel as the placement's gate, the other way round: the child closes its end once
    // its terminal is set up. @see Process::spawnProgress()
    auto settle = makeGate();
    if (!settle)
        return spawnFailure(settle.error().message());
    // The gate parks the program between its spawn and its exec() until the placement has moved it into its
    // own resource domain, so everything it forks from then on is born there too. @see ProcessPlacement.
    auto gate = makeGate();
    if (!gate)
        return spawnFailure(gate.error().message());

    auto spec = SpawnSpec {
        .argv = std::move(childArgv),
        .lookup = ExecLookup::SearchPath,
        .environment = std::move(childEnvironment),
        .launch =
            TerminalLaunch {
                // The login shell the trampoline falls back to where the program cannot be executed.
                .fallbackArgv = Process::loginShell(escape == SandboxEscape::Escapes),
                .workingDirectory = passesOwnEnvironment ? exec.workingDirectory.generic_string() : string {},
                .controllingTerminal =
                    Process::isFlatpak() ? ControllingTerminal::Skip : ControllingTerminal::Acquire,
                .terminalSettings = TerminalSettings::Configure,
                .failureReport = FailureReport::OnTerminal,
                .failureExit = FailureExit::Failure,
            },
    };
    spec.oomScoreAdjust = childOomScoreAdjust;
    auto fds = vector<InheritedFd> {
        { .slot = TrampolineSlot::Gate, .fd = gate->childEnd.get() },
        { .slot = TrampolineSlot::Settle, .fd = settle->childEnd.get() },
        { .slot = TrampolineSlot::Terminal, .fd = descriptors->terminal },
    };
    if (descriptors->stdoutFastPipeWriter)
        fds.push_back({ .slot = TrampolineSlot::FastPipe, .fd = *descriptors->stdoutFastPipeWriter });

    auto prepared = prepareTrampoline(std::move(spec), fds);
    if (!prepared)
        return spawnFailure(prepared.error().message());
    prepared->gate = std::move(gate->parentEnd);
    prepared->settle = std::move(settle->parentEnd);
    return std::move(*prepared);
}

StartResult Process::start()
{
    if (_d->origin == Private::Origin::Adopted)
        return {}; // Running already, on a PTY that is open.

    if (auto started = _d->pty->start(); !started)
        return started;

    auto prepared = prepareTrampolineSpawn(*_d->pty,
                                           ExecInfo { .program = _d->path,
                                                      .arguments = _d->args,
                                                      .workingDirectory = _d->cwd,
                                                      .env = _d->env,
                                                      .removedEnvironment = _d->removedEnvironment },
                                           sandboxEscapeFromConfig(_d->escapeSandbox),
                                           _d->placement->childOomScoreAdjust());
    if (!prepared)
        return std::unexpected(prepared.error());

    auto const spawned =
        launchTrampoline(*prepared).and_then([&](pid_t pid) -> std::expected<pid_t, std::error_code> {
            // Written after the spawn, not before: a spec larger than the socket's buffer is read as it is
            // written.
            if (auto written = writeSpawnSpec(*prepared); !written)
            {
                std::ignore = util::waitForExit(pid); // it read end-of-file as the socket closed
                return std::unexpected(written.error());
            }
            return pid;
        });
    if (!spawned)
        return std::unexpected(
            StartFailure { .error = StartError::SpawnFailed, .detail = spawned.error().message() });

    // Known before the master can read end-of-file: until closeSpawnDescriptors(), this process holds the
    // slave open, however soon the released program ends.
    core::locked(_d->exitStatusMutex, [&] { _d->pid = *spawned; });
    core::locked(_d->settleSignalMutex, [&] { _d->settleSignal = std::move(prepared->settle); });
    _d->placement->placeThenRelease(
        ParkedChild { *spawned, openPidFd(*spawned), std::move(prepared->gate), ProcessRole::Session });
    _d->pty->closeSpawnDescriptors();

    // Nothing to report: a working directory that cannot be entered, or a program that cannot be
    // executed, is discovered by the trampoline, which writes its own diagnostic onto the pty and exits.
    // The parent only fails when the spawn itself does.
    return {};
}

std::expected<void, AdoptError> Process::canAdopt(AdoptedChild const& child)
{
    auto const pid = static_cast<pid_t>(child.pid);
    // Only a child of ours can be waited for; this also rejects a recycled or foreign id.
    auto info = siginfo_t {};
    if (pid <= 0 || child.pid != static_cast<std::int64_t>(pid)
        || waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT) < 0)
        return std::unexpected(AdoptError::NoSuchChild);
    return {};
}

std::expected<std::unique_ptr<Process>, AdoptError> Process::adopt(AdoptedChild const& child,
                                                                   std::unique_ptr<Pty> pty)
{
    Require(pty != nullptr);
    if (auto const adoptable = canAdopt(child); !adoptable)
        return std::unexpected(adoptable.error());
    auto const pid = static_cast<pid_t>(child.pid);

    auto process = std::make_unique<Process>(child.executable,
                                             child.args,
                                             child.cwd,
                                             Environment {},
                                             std::vector<std::string> {},
                                             /*escapeSandbox=*/false,
                                             std::move(pty),
                                             std::make_shared<NoPlacement>(),
                                             child.hangupGrace);
    process->_d->origin = Private::Origin::Adopted;
    process->_d->pid = pid;
    return process;
}

std::optional<AdoptedChild> Process::handoverChild() const
{
    auto const _ = lock_guard { _d->exitStatusMutex };
    return _d->adoptedChildLocked();
}

std::optional<AdoptedChild> Process::releaseChild()
{
    auto const _ = lock_guard { _d->exitStatusMutex };
    auto child = _d->adoptedChildLocked();
    _d->pid = NoChild;
    return child;
}

Process::~Process()
{
    {
        auto const _ = lock_guard { _d->hangupWatchMutex };
        if (_d->hangupWatch)
            _d->hangupWatch->join();
    }
    (void) wait();
}

void Process::close()
{
    // The kernel hangs the child up as its terminal's master closes, sending its session SIGHUP. A child
    // that ignores that would keep everyone waiting for it waiting for good -- a session's exit watcher,
    // and ~Process on the GUI thread as the session is freed -- so after a short grace it is killed, as
    // wezterm does. Off this thread: a shell ends within the grace, and closing a tab must not wait on it.
    _d->pty->close();

    auto const _ = lock_guard { _d->hangupWatchMutex };
    if (_d->hangupWatch || !alive())
        return;
    // The grace starts here, not whenever the thread gets to run.
    auto const& clock = _d->hangupGrace.clock.get();
    _d->hangupWatch.emplace([this, &clock, deadline = clock.now() + _d->hangupGrace.duration]() {
        while (clock.now() < deadline)
        {
            if (!alive())
                return;
            std::this_thread::sleep_for(HangupGraceStep);
        }
        terminate(TerminationHint::Kill);
    });
}

Pty& Process::pty() noexcept
{
    return *_d->pty;
}

Pty const& Process::pty() const noexcept
{
    return *_d->pty;
}

void Process::waitForClosed()
{
    (void) wait();
}

optional<Process::ExitStatus> Process::checkStatus() const
{
    return _d->checkStatus(false);
}

optional<Process::ExitStatus> Process::Private::checkStatus(bool waitForExit) const
{
    auto childPid = NoChild;
    {
        auto const _ = lock_guard { exitStatusMutex };
        if (exitStatus.has_value())
            return exitStatus;
        childPid = pid;
    }

    // Without a child there is no status to ask for, and asking waitpid() about NoChild would wait for
    // any child of this process at all.
    if (childPid == NoChild)
        return nullopt;

    // A blocking wait only waits for the child to end here, without reaping it (WNOWAIT): every reap
    // happens below, under the lock, together with recording how the child ended. Reaped out here, a
    // concurrent waiter -- a session's exit watcher beside a close's grace -- could find the child gone
    // while its status was still unrecorded, and so report none.
    if (waitForExit)
    {
        auto info = siginfo_t {};
        while (waitid(P_PID, static_cast<id_t>(childPid), &info, WEXITED | WNOWAIT) < 0 && errno == EINTR)
        {
            // A signal arriving is no reason to stop waiting.
        }
        // Whatever ended the wait, the reap below says what became of the child.
    }

    auto const _ = lock_guard { exitStatusMutex };
    if (exitStatus.has_value())
        return exitStatus; // Reaped by a concurrent wait, which recorded it.
    if (pid == NoChild)
        return nullopt;

    auto status = 0;
    auto rv = pid_t {};
    do
    {
        rv = waitpid(pid, &status, WNOHANG);
    } while (rv < 0 && errno == EINTR);

    if (rv < 0)
    {
        if (errno == ECHILD)
        {
            // Not ours to wait for any more -- reaped by the system with SIGCHLD ignored -- so there is
            // no child, and no status to be had for it.
            pid = NoChild;
            return nullopt;
        }
        errorLog()("waitpid() failed: {}", std::generic_category().message(errno));
        return nullopt;
    }

    // Still running, or only stopped, as a tracer is told.
    if (rv == 0 || (!WIFEXITED(status) && !WIFSIGNALED(status)))
        return nullopt;

    pid = NoChild;
    exitStatus = WIFEXITED(status) ? ExitStatus { NormalExit { WEXITSTATUS(status) } }
                                   : ExitStatus { SignalExit { WTERMSIG(status) } };
    return exitStatus;
}

bool Process::alive() const noexcept
{
    return _d->child() != NoChild && !_d->checkStatus(false).has_value();
}

Process::SpawnProgress Process::spawnProgress() const
{
    auto const _ = lock_guard { _d->settleSignalMutex };
    return settleProgressOf(_d->settleSignal);
}

void Process::terminate(TerminationHint terminationHint)
{
    // The id is taken once, and kill() is handed nothing else: it reads 0 and -1 as whole groups of
    // processes, the terminal's own among them. A child that has already exited is not signalled, as its
    // id may have gone to another process -- though one reaped concurrently, between this check and the
    // kill(), still can be: closing that window takes a process descriptor, which POSIX does not have.
    auto const childPid = _d->child();
    if (childPid == NoChild || _d->checkStatus(false).has_value())
        return;

    ::kill(childPid, signalFor(terminationHint));
}

optional<Process::ExitStatus> Process::wait()
{
    return _d->checkStatus(true);
}

namespace
{
    /// Resolves the user's login shell, asking flatpak's host side when sandbox escape is requested.
    ///
    /// @param escapeSandbox Whether the shell is to be run outside the flatpak sandbox.
    /// @return argv of the login shell; `{"/bin/sh"}` if the user has no password-database entry.
    [[nodiscard]] vector<string> resolveLoginShell(bool escapeSandbox)
    {
        if (auto const pw = core::currentUserPasswordEntry(); pw.has_value())
        {
#ifdef __APPLE__
            core::ignoreUnused(escapeSandbox);
            return { pw->shell };
#else
            if (Process::isFlatpak() && escapeSandbox)
            {
                char buf[1024];
                auto const cmd = std::format("flatpak-spawn --host getent passwd {}", pw->name);
                FILE* fp = popen(cmd.c_str(), "r");
                auto fpCloser = core::Finally { [fp]() { pclose(fp); } };
                size_t const nread = fread(buf, sizeof(char), sizeof(buf) / sizeof(char), fp);
                auto const output = trimRight(string_view(buf, nread));
                auto const colonIndex = output.rfind(':');
                if (colonIndex != string_view::npos)
                {
                    auto const shell = output.substr(colonIndex + 1);
                    return { string(shell.data(), shell.size()) };
                }
            }

            return { pw->shell };
#endif
        }
        else
            return { "/bin/sh"s };
    }
} // namespace

vector<string> Process::loginShell(bool escapeSandbox)
{
    // Memoized, because the login shell cannot change over the process lifetime and start() resolves
    // it before *every* spawn (@see start()).
    // Recomputing per session would mean a getpwuid_r() each time, and under flatpak sandbox escape an
    // entire `flatpak-spawn --host getent passwd` subprocess. Each branch initializes on first use;
    // initialization of a function-local static is thread safe.
    if (escapeSandbox)
    {
        static auto const shell = resolveLoginShell(true);
        return shell;
    }

    static auto const shell = resolveLoginShell(false);
    return shell;
}

std::string Process::userName()
{
    if (auto const pw = core::currentUserPasswordEntry(); pw.has_value() && !pw->name.empty())
        return pw->name;

    if (auto const user = core::defaultEnvironment().get("USER"); user.has_value())
        return std::string { *user };

    return "unknown";
}

fs::path Process::homeDirectory()
{
    if (auto const home = core::defaultEnvironment().get("HOME"); home.has_value())
        return { *home };
    else if (auto const pw = core::currentUserPasswordEntry(); pw.has_value() && !pw->homeDirectory.empty())
        return { pw->homeDirectory };
    else
        return { "/" };
}

string Process::workingDirectory() const
{
    // Between fork() and the chdir() the trampoline does before it settles, the child is still in this
    // process's directory, which it is about to leave: the one it starts in is the answer.
    if (!_d->cwd.empty() && spawnProgress() == SpawnProgress::InProgress)
        return _d->cwd.string();

    auto const childPid = _d->child();
    if (childPid == NoChild)
        return "."s;
#ifdef __APPLE__
    // Asked of the terminal's foreground process group there, as it always was.
    auto const* const unixPty = dynamic_cast<UnixPty const*>(_d->pty.get());
    if (unixPty == nullptr)
        return "."s;
    return unixPty->foregroundProcessGroup().and_then(&util::workingDirectoryOf).value_or("."s);
#else
    return util::workingDirectoryOf(childPid).value_or("."s);
#endif
}

std::optional<std::string> Process::foregroundProcessName() const
{
    if (_d->child() == NoChild)
        return std::nullopt;
    // A Process may run on any Pty, so ask before taking it for a UnixPty.
    auto const* const unixPty = dynamic_cast<UnixPty const*>(_d->pty.get());
    if (unixPty == nullptr)
        return std::nullopt;
    return unixPty->foregroundProcessName();
}

// {{{ startDetached
std::expected<int, std::error_code> startDetached(DetachedCommand const& command, ProcessPlacement& placement)
{
    auto const lastError = [] {
        return std::unexpected(std::error_code { errno, std::generic_category() });
    };

    // The files the program's standard streams are to use are opened here, so that an input that cannot be
    // read is reported to the caller, not found by a child nobody hears from.
    auto const input = command.standardInput.empty()
                           ? OwnedFd {}
                           : OwnedFd { ::open(command.standardInput.c_str(), O_RDONLY | O_CLOEXEC) };
    if (!command.standardInput.empty() && !input.isOpen())
        return lastError();
    auto const discard = command.output == DetachedOutput::Discarded
                             ? OwnedFd { ::open("/dev/null", O_WRONLY | O_CLOEXEC) }
                             : OwnedFd {};
    if (command.output == DetachedOutput::Discarded && !discard.isOpen())
        return lastError();

    // The trampoline starts a session of its own, parks the program as Process::start() parks a session's
    // shell (@see ProcessPlacement), and leaves it to init, or to the nearest subreaper.
    auto spec = SpawnSpec {
        .image = command.image,
        .argv = command.argv.empty() ? vector<string> { command.image } : command.argv,
        .lookup = ExecLookup::ExactPath,
        .environment = command.environment,
        .launch = DetachedLaunch {},
    };
    auto fds = vector<InheritedFd> {};
    if (input.isOpen())
        fds.push_back({ .slot = TrampolineSlot::Input, .fd = input.get() });
    if (discard.isOpen())
        fds.push_back({ .slot = TrampolineSlot::Output, .fd = discard.get() });
    return spawnPlaced(std::move(spec),
                       fds,
                       placement,
                       command.role,
                       command.environment ? TrampolineEnvironment::Program
                                           : TrampolineEnvironment::Inherited);
}
// }}}

} // namespace vtpty
