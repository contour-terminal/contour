// SPDX-License-Identifier: Apache-2.0
#include <vtpty/CommandLine.hpp>
#include <vtpty/DetachedProcess.hpp>
#include <vtpty/EnvironmentBlock.hpp>
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

// POSIX requires no header to declare `environ`; each platform is left to differ, and all three we
// build on do.
//
// macOS: a shared library cannot link against the symbol directly, so it goes through
// _NSGetEnviron(). Linux: glibc's <unistd.h> declares it, so nothing is needed. FreeBSD: its
// <unistd.h> does NOT declare it in the configuration this tree builds with, which is why the
// FreeBSD job failed with "use of undeclared identifier 'environ'" -- so the declaration is
// supplied here, as POSIX intends. `extern "C"` rather than a bare `extern`: the symbol has C
// linkage, and declaring it with C++ linkage would name a different entity and fail to link.
#ifdef __APPLE__
    #include <crt_externs.h>
    #define environ (*_NSGetEnviron())
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
extern "C" char** environ;
#endif

using namespace std;
using namespace std::string_view_literals;
using core::trimRight;

namespace fs = std::filesystem;

namespace vtpty
{

namespace
{
    constexpr auto StdoutFastPipeFd = 3;
    constexpr auto StdoutFastPipeFdStr = "3"sv;
    constexpr auto StdoutFastPipeEnvironmentName = "STDOUT_FASTPIPE"sv;

    /// The id a Process holds while it has no child. @see Process::Private::pid.
    constexpr pid_t NoChild = -1;

    /// How often a HangupGrace looks whether the child has ended, and whether the grace has passed.
    constexpr auto HangupGraceStep = std::chrono::milliseconds { 10 };

    /// How long startDetached() waits, in milliseconds, for its intermediate process to name the program's:
    /// no time at all once it has reaped that process, which sent the name before it exited.
    constexpr auto DetachedHandoverTimeout = 5000;

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

    /// @return The current process environment's entries; none when it has been cleared.
    [[nodiscard]] vector<string_view> inheritedEnvironment()
    {
        auto entries = vector<string_view> {};
        if (environ == nullptr) // clearenv() leaves no array at all
            return entries;
        for (auto const index: std::views::iota(std::size_t { 0 })
                                   | std::views::take_while([](auto i) { return environ[i] != nullptr; }))
            entries.emplace_back(environ[index]);
        return entries;
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

StartResult Process::start()
{
    if (_d->origin == Private::Origin::Adopted)
        return {}; // Running already, on a PTY that is open.

    if (auto started = _d->pty->start(); !started)
        return started;

    UnixPipe* stdoutFastPipe = [this]() -> UnixPipe* {
        if (auto* p = dynamic_cast<UnixPty*>(_d->pty.get()))
            return &p->stdoutFastPipe();
        return nullptr;
    }();

    // When escaping the sandbox, flatpak-spawn is handed the variables as --env= arguments and the
    // child keeps our environment untouched.
    auto const host = programHost(sandboxEscapeFromConfig(_d->escapeSandbox));
    auto const passesOwnEnvironment = host == ProgramHost::ThisMachine;

    // The environment is assembled here, so that the trampoline can install it as it is.
    auto childEnvironment = [stdoutFastPipe, passesOwnEnvironment, this]() -> vector<string> {
        if (!passesOwnEnvironment)
            return {};

        auto overrides = Environment {};
        if (isFlatpak())
            overrides["TERMINFO"] = "/app/share/terminfo";
        for (auto const& [name, value]: _d->env)
            overrides[name] = value;
        if (stdoutFastPipe)
            overrides[string { StdoutFastPipeEnvironmentName }] = string { StdoutFastPipeFdStr };
        return buildEnvironmentBlock(
            inheritedEnvironment(), overrides, _d->removedEnvironment, EnvironmentNames::CaseSensitive);
    }();

    // What runs: the program as configured -- or, where it names no program but holds whitespace
    // ("bash -l"), the command line it spells -- looked up on the PATH the child execs with, a relative
    // path in the directory the child changes to before it execs; looked up nowhere when flatpak-spawn
    // runs it on the host (programsFor()).
    auto const searchPath = string {
        environmentValue(childEnvironment, "PATH", EnvironmentNames::CaseSensitive).value_or(string_view {})
    };
    auto const invocation = invocationToRun(
        _d->path,
        _d->args,
        programsFor(host,
                    ProgramSearch { .path = searchPath,
                                    .workingDirectory = _d->cwd,
                                    .fileSystem = core::platform::NativeFileSystem::instance() }));

    auto childArgv = [stdoutFastPipe, host, &invocation, this]() -> vector<string> {
        if (host == ProgramHost::ThisMachine)
        {
            auto args = vector<string> { invocation.program };
            args.insert(args.end(), invocation.arguments.begin(), invocation.arguments.end());
            return args;
        }

        auto const terminfoBaseDirectory = homeDirectory() / ".var/app/org.contourterminal.Contour/terminfo";

        // Prepend flatpak to jump out of sandbox:
        // flatpak-spawn --host --watch-bus --env=TERM=$TERM /bin/zsh
        auto realArgs = vector<string> { "/usr/bin/flatpak-spawn" };
        realArgs.emplace_back("--host");
        realArgs.emplace_back("--watch-bus");
        realArgs.emplace_back(std::format("--env=TERMINFO={}", terminfoBaseDirectory.generic_string()));
        if (stdoutFastPipe)
        {
            realArgs.emplace_back(std::format("--forward-fd={}", StdoutFastPipeFdStr));
            realArgs.emplace_back(
                std::format("--env={}={}", StdoutFastPipeEnvironmentName, StdoutFastPipeFdStr));
        }
        if (!_d->cwd.empty())
            realArgs.emplace_back(std::format("--directory={}", _d->cwd.generic_string()));
        realArgs.emplace_back(std::format("--env=TERM={}", "contour"));
        for (auto&& [name, value]: _d->env)
            realArgs.emplace_back(std::format("--env={}={}", name, value));
        if (stdoutFastPipe)
            realArgs.emplace_back(
                std::format("--env={}={}", StdoutFastPipeEnvironmentName, StdoutFastPipeFd));
        realArgs.push_back(invocation.program);
        for (auto const& arg: invocation.arguments)
            realArgs.push_back(arg);

        return realArgs;
    }();

    // The login shell the trampoline falls back to where the program cannot be executed.
    auto const loginShellArgs = loginShell(_d->escapeSandbox);

    // The gate parks the child between its spawn and exec() until the placement has moved it into its
    // own resource domain, so everything it forks from then on is born there too. @see ProcessPlacement.
    auto gate = makeGate();
    if (!gate)
        return std::unexpected(
            StartFailure { .error = StartError::SpawnFailed, .detail = gate.error().message() });
    // The same kind of channel, the other way round: the child closes its end once its terminal is set up.
    // @see Process::spawnProgress()
    auto settle = makeGate();
    if (!settle)
        return std::unexpected(
            StartFailure { .error = StartError::SpawnFailed, .detail = settle.error().message() });
    auto* const unixPty = dynamic_cast<UnixPty*>(_d->pty.get());
    if (unixPty == nullptr)
        return std::unexpected(StartFailure { .error = StartError::SpawnFailed,
                                              .detail = "The terminal is not a Unix pseudo terminal." });

    auto const spec = SpawnSpec {
        .mode = SpawnMode::Terminal,
        .argv = std::move(childArgv),
        .lookup = ExecLookup::SearchPath,
        .fallbackArgv = loginShellArgs,
        .fallbackText = core::joinHumanReadableQuoted(loginShellArgs, " "),
        .workingDirectory = passesOwnEnvironment ? _d->cwd.generic_string() : string {},
        .environment = passesOwnEnvironment ? std::optional { std::move(childEnvironment) } : std::nullopt,
        .oomScoreAdjust = _d->placement->childOomScoreAdjust(),
        .controllingTerminal = isFlatpak() ? ControllingTerminal::Skip : ControllingTerminal::Acquire,
        .terminalSettings = TerminalSettings::Configure,
        .failureReport = FailureReport::OnTerminal,
    };
    auto fds = vector<InheritedFd> {
        { TrampolineSlot::Gate, gate->childEnd.get() },
        { TrampolineSlot::Settle, settle->childEnd.get() },
        { TrampolineSlot::Terminal, unbox<int>(unixPty->slaveHandle()) },
    };
    if (stdoutFastPipe && stdoutFastPipe->writer() != -1)
        fds.push_back({ TrampolineSlot::FastPipe, stdoutFastPipe->writer() });

    auto const spawned = spawnTrampoline(spec, fds);
    if (!spawned)
        return std::unexpected(
            StartFailure { .error = StartError::SpawnFailed, .detail = spawned.error().message() });
    auto const child = *spawned;

    core::locked(_d->exitStatusMutex, [&] { _d->pid = child; });
    gate->childEnd.reset();
    settle->childEnd.reset();
    core::locked(_d->settleSignalMutex, [&] { _d->settleSignal = std::move(settle->parentEnd); });
    _d->pty->slave().close();
    if (stdoutFastPipe)
        stdoutFastPipe->closeWriter();
    _d->placement->placeThenRelease(
        ParkedChild { child, openPidFd(child), std::move(gate->parentEnd), ProcessRole::Session });

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
    if (!_d->settleSignal.isOpen())
        return SpawnProgress::Settled;
    // Readable means settled: the child's byte, or end-of-file as it exited before. Without waiting.
    auto pollFd = pollfd { .fd = _d->settleSignal.get(), .events = POLLIN, .revents = 0 };
    auto ready = 0;
    while ((ready = ::poll(&pollFd, 1, 0)) < 0 && errno == EINTR)
        ;
    if (ready == 0)
        return SpawnProgress::InProgress;
    _d->settleSignal.reset();
    return SpawnProgress::Settled;
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
#ifdef __linux__
    auto const childPid = _d->child();
    if (childPid == NoChild)
        return "."s;

    try
    {
        auto const path = fs::path { std::format("/proc/{}/cwd", childPid) };
        auto const cwd = fs::read_symlink(path);
        return cwd.string();
    }
    catch (...)
    {
        // ignore failure, and use default instead.
        return "."s;
    }
#elifdef __APPLE__
    try
    {
        // A Process may run on any Pty, so ask before taking it for a UnixPty.
        auto const* const unixPty = dynamic_cast<UnixPty const*>(_d->pty.get());
        if (unixPty == nullptr)
            return "."s;

        auto vpi = proc_vnodepathinfo {};
        auto const pid = tcgetpgrp(unbox<int>(unixPty->handle()));

        if (proc_pidinfo(pid, PROC_PIDVNODEPATHINFO, 0, &vpi, sizeof(vpi)) <= 0)
            return "."s;

        return string(vpi.pvi_cdir.vip_path);
    }
    catch (...)
    {
        return "."s;
    }
#else
    // e.g. FreeBSD
    return "."s;
#endif
}

std::optional<std::string> Process::foregroundProcessName() const
{
#if defined(__linux__) || defined(__APPLE__)
    if (_d->child() == NoChild)
        return std::nullopt;
    // A Process may run on any Pty, so ask before taking it for a UnixPty.
    auto const* const unixPty = dynamic_cast<UnixPty const*>(_d->pty.get());
    if (unixPty == nullptr)
        return std::nullopt;
    auto const group = tcgetpgrp(unbox<int>(unixPty->handle()));
    if (group <= 0)
        return std::nullopt;
    #ifdef __linux__
    // Streamed: /proc reports a size of zero, so a read sized by the file's length reads nothing.
    auto in = std::ifstream { std::format("/proc/{}/comm", group) };
    auto name = std::string {};
    if (!std::getline(in, name) || name.empty())
        return std::nullopt;
    return name;
    #else
    auto name = std::array<char, 2 * MAXCOMLEN + 1> {};
    if (proc_name(group, name.data(), static_cast<uint32_t>(name.size())) <= 0)
        return std::nullopt;
    return std::string { name.data() };
    #endif
#else
    return std::nullopt;
#endif
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

    auto gate = makeGate();
    if (!gate)
        return std::unexpected(gate.error());

    // The trampoline starts a session of its own, parks the program as Process::start() parks a session's
    // shell (@see ProcessPlacement), tells the gate the program's id, and exits: the program is left to
    // init, or to the nearest subreaper, to wait for -- not to this process.
    auto const spec = SpawnSpec {
        .mode = SpawnMode::Detached,
        .image = command.image,
        .argv = command.argv.empty() ? vector<string> { command.image } : command.argv,
        .lookup = ExecLookup::ExactPath,
        .oomScoreAdjust = placement.childOomScoreAdjust(),
    };
    auto fds = vector<InheritedFd> { { TrampolineSlot::Gate, gate->childEnd.get() } };
    if (input.isOpen())
        fds.push_back({ TrampolineSlot::Input, input.get() });
    if (discard.isOpen())
        fds.push_back({ TrampolineSlot::Output, discard.get() });

    auto const trampoline = spawnTrampoline(spec, fds);
    if (!trampoline)
        return std::unexpected(trampoline.error());

    gate->childEnd.reset();
    auto status = 0;
    while (::waitpid(*trampoline, &status, 0) < 0 && errno == EINTR)
        ;
    if (WIFEXITED(status))
        switch (static_cast<TrampolineExit>(WEXITSTATUS(status)))
        {
            case TrampolineExit::UnsupportedVersion:
                return std::unexpected(std::make_error_code(std::errc::protocol_not_supported));
            case TrampolineExit::SpecUnreadable:
                return std::unexpected(std::make_error_code(std::errc::bad_message));
        }
    // Once the trampoline is reaped, what it sent is there to read, or never comes. Waited for all the
    // same, briefly, should a SIGCHLD handler have reaped it first; a program left waiting on the gate
    // then runs unplaced, as the gate's end closes on the way out.
    auto ready = pollfd { .fd = gate->parentEnd.get(), .events = POLLIN, .revents = 0 };
    while (::poll(&ready, 1, DetachedHandoverTimeout) < 0 && errno == EINTR)
        ;
    auto received = std::array<char, sizeof(pid_t)> {};
    auto const length = ::recv(gate->parentEnd.get(), received.data(), received.size(), MSG_DONTWAIT);
    auto const pid = std::bit_cast<pid_t>(received);
    if (std::cmp_not_equal(length, received.size()) || pid == 0)
        return std::unexpected(std::make_error_code(std::errc::no_child_process));
    if (pid < 0)
        return std::unexpected(std::error_code { -pid, std::generic_category() });

    // Not this process's child, yet its pid still names it: it waits on the gate, and only a signal from
    // elsewhere could end it before the pidfd is opened.
    placement.placeThenRelease(ParkedChild { pid, openPidFd(pid), std::move(gate->parentEnd), command.role });
    return pid;
}
// }}}

} // namespace vtpty
