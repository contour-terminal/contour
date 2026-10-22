// SPDX-License-Identifier: Apache-2.0
#include <vtpty/CommandLine.hpp>
#include <vtpty/DetachedProcess.hpp>
#include <vtpty/EnvironmentBlock.hpp>
#include <vtpty/ErrnoText.hpp>
#include <vtpty/Process.hpp>
#include <vtpty/ProcessPlacement.hpp>
#include <vtpty/Pty.hpp>
#include <vtpty/SandboxInfo.hpp>
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
#include <cstring>
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

    string getLastErrorAsString()
    {
        return std::generic_category().message(errno);
    }

    // {{{ async-signal-safe helpers, for use between fork() and exec()
    //
    // Everything in this block is called from the forked child before it has called exec(). The child of
    // a multi-threaded process may call only async-signal-safe functions until it execs (fork(2) NOTES;
    // POSIX XSH fork()): it shares the parent's memory image, and a lock another thread held at the fork
    // may be held in the child. So no printf, no strerror (errnoText() instead), no setenv, and no
    // allocation.

    /// Writes @p text to @p fd in full, retrying on short writes and EINTR.
    ///
    /// @param fd File descriptor to write to.
    /// @param text Bytes to write.
    void writeAll(int fd, string_view text) noexcept
    {
        while (!text.empty())
        {
            auto const written = ::write(fd, text.data(), text.size());
            if (written > 0)
                text.remove_prefix(static_cast<size_t>(written));
            else if (written < 0 && errno == EINTR)
                continue; // Interrupted before any byte was written: retry.
            else
                break; // NB: errno is only meaningful for written < 0. A write() returning 0 did not
                       // fail and did not set it, so retrying on a stale EINTR left over from an
                       // earlier call -- the child inherits the parent's errno -- would spin forever.
        }
    }

    /// Writes @p value to this process's own oom_score_adj; a failure is ignored.
    ///
    /// @param value The text to write, prepared before fork(); nothing is written when empty.
    void writeOwnOomScoreAdjust(string_view value) noexcept
    {
        if (value.empty())
            return;
        auto const fd = ::open("/proc/self/oom_score_adj", O_WRONLY | O_CLOEXEC);
        if (fd < 0)
            return;
        writeAll(fd, value);
        ::close(fd);
    }

    // }}}

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

    /// Makes @p b refer to what @p a does, and survive an exec, retrying dup2() on EBUSY and EINTR.
    ///
    /// Where @p a already is @p b -- this process's standard input closed, a descriptor opened
    /// close-on-exec lands on 0 -- dup2() returns without changing anything (POSIX), the close-on-exec
    /// flag included, and the exec would close it. The flag is cleared instead, with fcntl(), which is
    /// async-signal-safe.
    /// @param a The descriptor to hand over.
    /// @param b The descriptor the exec keeps.
    void saveDup2(int a, int b) noexcept
    {
        if (a == b)
        {
            if (auto const flags = ::fcntl(a, F_GETFD); flags != -1)
                (void) ::fcntl(a, F_SETFD, flags & ~FD_CLOEXEC);
            return;
        }
        while (dup2(a, b) == -1 && (errno == EBUSY || errno == EINTR))
            ;
    }

    /// Hands the fast pipe's writer to the program as StdoutFastPipeFd, and closes the pipe's own
    /// descriptors: the reader first, as it may sit on StdoutFastPipeFd itself, which the writer then takes;
    /// the writer only where it is not StdoutFastPipeFd already. Async-signal-safe.
    /// @param pipe The fast pipe; nothing is done where it has no writer.
    void handOverFastPipe(UnixPipe& pipe) noexcept
    {
        auto const writer = pipe.writer();
        if (writer == -1)
            return;
        pipe.closeReader();
        saveDup2(writer, StdoutFastPipeFd);
        if (writer != StdoutFastPipeFd)
            pipe.closeWriter();
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

    // The child's id, or NoChild: before start() spawned one, when fork() failed, and once the child has
    // been reaped. Never 0, which waitpid() and kill() read as the caller's whole process group, nor any
    // other id they would read as more than one process. Written under exitStatusMutex.
    mutable pid_t pid = NoChild;
    mutable std::mutex exitStatusMutex {};
    mutable std::optional<Process::ExitStatus> exitStatus {};

    /// Started by close() while the child runs: kills it once hangupGrace has passed without it ending.
    std::optional<std::thread> hangupWatch {};
    std::mutex hangupWatchMutex {}; ///< Guards hangupWatch.

    [[nodiscard]] std::optional<ExitStatus> checkStatus(bool waitForExit) const;

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

StartResult Process::start()
{
    if (auto started = _d->pty->start(); !started)
        return started;

    UnixPipe* stdoutFastPipe = [this]() -> UnixPipe* {
        if (auto* p = dynamic_cast<UnixPty*>(_d->pty.get()))
            return &p->stdoutFastPipe();
        return nullptr;
    }();

    // When escaping the sandbox, flatpak-spawn is handed the variables as --env= arguments and the
    // child keeps our environment untouched.
    auto const passesOwnEnvironment = !isFlatpak() || !_d->escapeSandbox;

    // The environment is assembled here, before the fork, so that the child can install it with a
    // single pointer store: setenv() after fork() is neither async-signal-safe nor thread-safe.
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

    // `environ` layout: pointers to each entry, terminated by a null pointer. Both vectors outlive
    // the child's exec(), and the parent destroys them on the way out of this function.
    auto childEnvp = vector<char*> {};
    childEnvp.reserve(childEnvironment.size() + 1);
    for (auto& entry: childEnvironment)
        childEnvp.push_back(entry.data());
    childEnvp.push_back(nullptr);

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
        programsFor(isFlatpak() && _d->escapeSandbox ? ProgramHost::SandboxHost : ProgramHost::ThisMachine,
                    ProgramSearch { .path = searchPath,
                                    .workingDirectory = _d->cwd,
                                    .fileSystem = core::platform::NativeFileSystem::instance() }));

    // argv is assembled before the fork for the same reason: the flatpak variant reads the home
    // directory and formats a dozen strings, none of which the child is allowed to do.
    auto childArgs = [stdoutFastPipe, &invocation, this]() -> vector<string> {
        if (!isFlatpak() || !_d->escapeSandbox)
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

    auto childArgv = vector<char*> {};
    childArgv.reserve(childArgs.size() + 1);
    for (auto& entry: childArgs)
        childArgv.push_back(entry.data());
    childArgv.push_back(nullptr);

    // The login-shell fallback is likewise prepared before the fork. Between fork() and exec() the
    // child may call async-signal-safe functions only, and loginShell() reaches getpwuid_r() -- NSS,
    // locks, allocation -- while joining the arguments for the diagnostic allocates as well. Running
    // any of that in a child forked from the multi-threaded GUI process can deadlock outright, if a
    // thread happened to hold the malloc arena or the NSS lock at the moment of the fork.
    auto loginShellArgs = loginShell(_d->escapeSandbox);
    auto const loginShellText = core::joinHumanReadableQuoted(loginShellArgs, " ");

    // Same layout and lifetime as childEnvp above: the vector's own storage backs the argv, so
    // nothing needs freeing in either process.
    auto loginShellArgv = vector<char*> {};
    if (!loginShellArgs.empty())
    {
        loginShellArgv.reserve(loginShellArgs.size() + 1);
        for (auto& entry: loginShellArgs)
            loginShellArgv.push_back(entry.data());
        loginShellArgv.push_back(nullptr);
    }

    // The gate parks the child between fork() and exec() until the placement has moved it into its
    // own resource domain, so everything it forks from then on is born there too. @see ProcessPlacement.
    auto gate = makeGate();
    if (!gate)
        return std::unexpected(
            StartFailure { .error = StartError::SpawnFailed, .detail = gate.error().message() });
    // Formatted before the fork: the child may not allocate. Nor may it ask sysconf(), which POSIX.1-2008
    // took off the async-signal-safe list.
    auto const oomScoreAdjust = _d->placement->childOomScoreAdjust()
                                    .transform([](int value) { return std::to_string(value); })
                                    .value_or(string {});
    auto const descriptorLimit = util::openDescriptorLimit();
    // Built here too: generic_string() builds a new string, on the heap for most working directories.
    auto const cwd = _d->cwd.generic_string();

    auto const forked = fork();

    switch (forked)
    {
        default: // in parent
            core::locked(_d->exitStatusMutex, [&] { _d->pid = forked; });
            gate->childEnd.reset();
            _d->pty->slave().close();
            if (stdoutFastPipe)
                stdoutFastPipe->closeWriter();
            _d->placement->placeThenRelease(
                ParkedChild { forked, openPidFd(forked), std::move(gate->parentEnd), ProcessRole::Session });
            break;
        case -1: // fork error: there is no child, and pid keeps saying so
            return std::unexpected(
                StartFailure { .error = StartError::SpawnFailed, .detail = getLastErrorAsString() });
        case 0: // in child
        {
            // Parked first, before the setup below can dup2() over the gate's descriptors. Only
            // async-signal-safe calls, on values prepared before the fork.
            ::close(gate->parentEnd.get());
            writeOwnOomScoreAdjust(oomScoreAdjust);
            awaitGateRelease(gate->childEnd.get());
            ::close(gate->childEnd.get());

            (void) _d->pty->slave().login();

            if (passesOwnEnvironment)
            {
                if (!cwd.empty() && chdir(cwd.c_str()) != 0)
                {
                    // Read before the writes that report it, any of which may set errno.
                    auto const failure = errno;
                    auto errorTextBuffer = array<char, 256> {};
                    writeAll(STDOUT_FILENO, "Failed to chdir to \"");
                    writeAll(STDOUT_FILENO, cwd);
                    writeAll(STDOUT_FILENO, "\". ");
                    writeAll(STDOUT_FILENO, errnoText(failure, errorTextBuffer));
                    writeAll(STDOUT_FILENO, "\n");
                    ::_exit(EXIT_FAILURE);
                }

                environ = childEnvp.data();
            }

            if (auto* pty = dynamic_cast<UnixPty*>(_d->pty.get()))
                handOverFastPipe(pty->stdoutFastPipe());

            util::closeDescriptorsAbove(StdoutFastPipeFd, descriptorLimit);

            // reset signal(s) to default that may have been changed in the parent process.
            signal(SIGPIPE, SIG_DFL);

            // execvp() is not on POSIX's async-signal-safe list, and is tolerated here: glibc, musl and the
            // BSDs search PATH in buffers on the stack; invocationToRun() has usually resolved the program to
            // a path with a slash already, which execvp() runs without a search; and its fallback of running
            // a file the kernel cannot execute (ENOEXEC) with sh is behaviour users rely on.
            ::execvp(childArgv[0], childArgv.data());
            // Read before the writes below, any of which may set errno; replaced by the login shell's.
            auto failure = errno;

            // Fallback: Try login shell. Everything this needs was computed before the fork.
            writeAll(STDOUT_FILENO, "\r\033[31;1mFailed to spawn ");
            writeAll(STDOUT_FILENO, childArgv[0]);
            writeAll(STDOUT_FILENO, "\033[m\r\nTrying login shell: ");
            writeAll(STDOUT_FILENO, loginShellText);
            writeAll(STDOUT_FILENO, "\n");
            if (!loginShellArgv.empty())
            {
                ::execvp(loginShellArgv[0], loginShellArgv.data()); // tolerated as the execvp() above is
                failure = errno;
            }

            // Bad luck.
            auto errorTextBuffer = array<char, 256> {};
            writeAll(STDOUT_FILENO, "\r\nOut of luck. ");
            writeAll(STDOUT_FILENO, errnoText(failure, errorTextBuffer));
            writeAll(STDOUT_FILENO, "\r\n\n");
            ::_exit(EXIT_FAILURE);
            break;
        }
    }

    // Nothing to report: on POSIX a working directory that cannot be entered, or a program that
    // cannot be executed, is discovered in the CHILD, which writes its own diagnostic onto the pty
    // (see the fallback ladder above) and exits. The parent only fails when fork() itself does.
    return {};
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
    // Memoized, because the login shell cannot change over the process lifetime and start() has to
    // resolve it before *every* fork -- the child, between fork() and exec(), may not (@see start()).
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

    // Everything the two forked processes use is prepared here: between fork() and exec() they may not
    // allocate. The files their standard streams are to use are opened here too, so that an input that
    // cannot be read is reported to the caller, not found by a child nobody hears from.
    auto arguments = command.argv.empty() ? vector<string> { command.image } : command.argv;
    auto argv = vector<char*> {};
    argv.reserve(arguments.size() + 1);
    for (auto& argument: arguments)
        argv.push_back(argument.data());
    argv.push_back(nullptr);

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

    auto const oomScoreAdjust = placement.childOomScoreAdjust()
                                    .transform([](int value) { return std::to_string(value); })
                                    .value_or(string {});
    auto const descriptorLimit = util::openDescriptorLimit();
    auto gate = makeGate();
    if (!gate)
        return std::unexpected(gate.error());

    auto const intermediate = ::fork();
    if (intermediate < 0)
        return lastError();
    if (intermediate == 0)
    {
        // A session of its own, the program's process, and gone: the program is left to init, or to the
        // nearest subreaper, to wait for -- not to this process. Async-signal-safe calls only.
        ::setsid();
        auto const program = ::fork();
        if (program == 0)
        {
            // Parked first, as Process::start() parks a session's shell. @see ProcessPlacement.
            ::close(gate->parentEnd.get());
            writeOwnOomScoreAdjust(oomScoreAdjust);
            awaitGateRelease(gate->childEnd.get());
            ::close(gate->childEnd.get());
            if (input.isOpen())
                saveDup2(input.get(), STDIN_FILENO);
            if (discard.isOpen())
            {
                saveDup2(discard.get(), STDOUT_FILENO);
                saveDup2(discard.get(), STDERR_FILENO);
            }
            util::closeDescriptorsAbove(STDERR_FILENO, descriptorLimit);
            // Neither what the caller ignores nor what its thread blocks -- the daemon blocks SIGINT and
            // SIGTERM -- carries over into the program, as exec() would carry both.
            signal(SIGPIPE, SIG_DFL);
            auto noSignals = sigset_t {};
            sigemptyset(&noSignals);
            pthread_sigmask(SIG_SETMASK, &noSignals, nullptr);
            ::execve(command.image.c_str(), argv.data(), environ);
            ::_exit(127);
        }
        // Which process to place travels over the gate: written into the end the program waits on, it
        // arrives at the end the parent holds. A failed fork sends its errno, negated.
        auto const placed = std::bit_cast<std::array<char, sizeof(pid_t)>>(program > 0 ? program : -errno);
        writeAll(gate->childEnd.get(), string_view { placed.data(), placed.size() });
        ::_exit(program > 0 ? EXIT_SUCCESS : EXIT_FAILURE);
    }

    gate->childEnd.reset();
    auto status = 0;
    while (::waitpid(intermediate, &status, 0) < 0 && errno == EINTR)
        ;
    // Once the intermediate is reaped, what it sent is there to read, or never comes. Waited for all the
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
