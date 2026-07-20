// SPDX-License-Identifier: Apache-2.0
#include <vtpty/Process.h>
#include <vtpty/Pty.h>
#include <vtpty/UnixPty.h>

#include <crispy/environment.h>
#include <crispy/overloaded.h>
#include <crispy/user_info.h>
#include <crispy/utils.h>

#include <array>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>

#ifndef __FreeBSD__
    #include <utmp.h>
#endif

#ifdef __APPLE__
    #include <libproc.h>
    #include <util.h>
#endif

#ifdef __linux__
    #include <pty.h>
#endif

#include <sys/types.h>
#include <sys/wait.h>

#include <csignal>

#include <unistd.h>

// A shared library on macOS cannot link against `environ` directly; everywhere else <unistd.h>
// already declares it.
#ifdef __APPLE__
    #include <crt_externs.h>
    #define environ (*_NSGetEnviron())
#endif

using namespace std;
using namespace std::string_view_literals;
using crispy::trimRight;

namespace fs = std::filesystem;

namespace vtpty
{

namespace
{
    constexpr auto StdoutFastPipeFd = 3;
    constexpr auto StdoutFastPipeFdStr = "3"sv;
    constexpr auto StdoutFastPipeEnvironmentName = "STDOUT_FASTPIPE"sv;

    string getLastErrorAsString()
    {
        return std::generic_category().message(errno);
    }

    // {{{ async-signal-safe helpers, for use between fork() and exec()
    //
    // Everything in this block is called from the forked child before it has called exec(). Only
    // async-signal-safe operations are permitted there: the child shares the parent's memory
    // image, and any lock (notably the malloc arena) that another parent thread happened to hold
    // at the moment of fork() is held forever in the child. So no printf, no strerror, no
    // setenv, and no allocation.

    /// Normalizes the two incompatible strerror_r() flavours to one shape.
    ///
    /// @param result What strerror_r() answered: a message pointer (GNU) or a status code (XSI).
    /// @param buffer The buffer strerror_r() was given.
    /// @return The message.
    template <typename Result>
    char const* strerrorResult(Result result, char* buffer) noexcept
    {
        if constexpr (std::is_pointer_v<Result>)
            return result; // GNU: answers a pointer, which may or may not be `buffer`.
        else
            return result == 0 ? buffer : "unknown error"; // XSI: fills `buffer`.
    }

    /// Describes an errno value without allocating, unlike strerror() and std::generic_category().
    ///
    /// @param errorCode errno value to describe.
    /// @param buffer Scratch space for the message; must outlive the returned pointer.
    /// @return Human-readable description of @p errorCode.
    [[nodiscard]] char const* errnoText(int errorCode, std::span<char> buffer) noexcept
    {
        return strerrorResult(::strerror_r(errorCode, buffer.data(), buffer.size()), buffer.data());
    }

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
    // }}}

    /// Builds the child's environment as "NAME=VALUE" entries: the current environment, with
    /// @p overrides replacing same-named entries.
    ///
    /// @param overrides Variables to add to, or replace in, the inherited environment.
    /// @return The entries, in no particular order.
    [[nodiscard]] vector<string> createEnvironmentBlock(Process::Environment const& overrides)
    {
        auto entries = vector<string> {};
        for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry)
        {
            auto const line = string_view { *entry };
            auto const separator = line.find('=');
            auto const name = separator != string_view::npos ? line.substr(0, separator) : line;
            if (!overrides.contains(string { name }))
                entries.emplace_back(line);
        }
        for (auto const& [name, value]: overrides)
            entries.emplace_back(std::format("{}={}", name, value));
        return entries;
    }

    void saveDup2(int a, int b)
    {
        while (dup2(a, b) == -1 && (errno == EBUSY || errno == EINTR))
            ;
    }

    void closeAllFileDescriptorsAbove(int keepFd)
    {
#ifdef HAVE_CLOSE_RANGE
        if (close_range(keepFd + 1, ~0U, 0) == 0)
            return;
#endif

#ifdef __linux__
        try
        {
            for (auto const& entry: fs::directory_iterator("/proc/self/fd"))
            {
                int const fd = std::stoi(entry.path().filename().string());
                if (fd > keepFd)
                    ::close(fd);
            }
            return;
        }
        catch (...) // NOLINT(bugprone-empty-catch)
        {
            // ignore
        }
#endif

        // Fallback: close up to sysconf(_SC_OPEN_MAX)
        int const maxFd = static_cast<int>(sysconf(_SC_OPEN_MAX));
        for (int i = keepFd + 1; i < maxFd; ++i)
            ::close(i);
    }
} // anonymous namespace

struct Process::Private
{
    string path;
    vector<string> args;
    fs::path cwd;
    Environment env;
    bool escapeSandbox;

    unique_ptr<Pty> pty {};
    mutable pid_t pid {};
    mutable std::mutex exitStatusMutex {};
    mutable std::optional<Process::ExitStatus> exitStatus {};

    [[nodiscard]] std::optional<ExitStatus> checkStatus(bool waitForExit) const;
};

Process::Process(string const& path,
                 vector<string> const& args,
                 fs::path const& cwd,
                 Environment const& env,
                 bool escapeSandbox,
                 unique_ptr<Pty> pty):
    _d(new Private { .path = path,
                     .args = args,
                     .cwd = cwd,
                     .env = env,
                     .escapeSandbox = escapeSandbox,
                     .pty = std::move(pty) },
       [](Private* p) { delete p; })
{
}

bool Process::isFlatpak()
{
    static bool const check = fs::exists("/.flatpak-info");
    return check;
}

void Process::start()
{
    _d->pty->start();

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
        return createEnvironmentBlock(overrides);
    }();

    // `environ` layout: pointers to each entry, terminated by a null pointer. Both vectors outlive
    // the child's exec(), and the parent destroys them on the way out of this function.
    auto childEnvp = vector<char*> {};
    childEnvp.reserve(childEnvironment.size() + 1);
    for (auto& entry: childEnvironment)
        childEnvp.push_back(entry.data());
    childEnvp.push_back(nullptr);

    // argv is assembled before the fork for the same reason: the flatpak variant reads the home
    // directory and formats a dozen strings, none of which the child is allowed to do.
    auto childArgs = [stdoutFastPipe, this]() -> vector<string> {
        if (!isFlatpak() || !_d->escapeSandbox)
        {
            auto args = vector<string> { _d->path };
            args.insert(args.end(), _d->args.begin(), _d->args.end());
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
        realArgs.push_back(_d->path);
        for (auto const& arg: _d->args)
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
    auto const loginShellText = crispy::joinHumanReadableQuoted(loginShellArgs, ' ');

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

    _d->pid = fork();

    switch (_d->pid)
    {
        default: // in parent
            _d->pty->slave().close();
            if (stdoutFastPipe)
                stdoutFastPipe->closeWriter();
            break;
        case -1: // fork error
            throw runtime_error { getLastErrorAsString() };
        case 0: // in child
        {
            (void) _d->pty->slave().login();

            auto const& cwd = _d->cwd.generic_string();
            if (passesOwnEnvironment)
            {
                if (!_d->cwd.empty() && chdir(cwd.c_str()) != 0)
                {
                    auto errorTextBuffer = array<char, 256> {};
                    writeAll(STDOUT_FILENO, "Failed to chdir to \"");
                    writeAll(STDOUT_FILENO, cwd);
                    writeAll(STDOUT_FILENO, "\". ");
                    writeAll(STDOUT_FILENO, errnoText(errno, errorTextBuffer));
                    writeAll(STDOUT_FILENO, "\n");
                    ::_exit(EXIT_FAILURE);
                }

                environ = childEnvp.data();
            }

            if (auto* pty = dynamic_cast<UnixPty*>(_d->pty.get()))
            {
                if (pty->stdoutFastPipe().writer() != -1)
                {
                    saveDup2(pty->stdoutFastPipe().writer(), StdoutFastPipeFd);
                    pty->stdoutFastPipe().close();
                }
            }

            closeAllFileDescriptorsAbove(StdoutFastPipeFd);

            // reset signal(s) to default that may have been changed in the parent process.
            signal(SIGPIPE, SIG_DFL);

            ::execvp(childArgv[0], childArgv.data());

            // Fallback: Try login shell. Everything this needs was computed before the fork.
            writeAll(STDOUT_FILENO, "\r\033[31;1mFailed to spawn ");
            writeAll(STDOUT_FILENO, childArgv[0]);
            writeAll(STDOUT_FILENO, "\033[m\r\nTrying login shell: ");
            writeAll(STDOUT_FILENO, loginShellText);
            writeAll(STDOUT_FILENO, "\n");
            if (!loginShellArgv.empty())
                ::execvp(loginShellArgv[0], loginShellArgv.data());

            // Bad luck.
            auto errorTextBuffer = array<char, 256> {};
            writeAll(STDOUT_FILENO, "\r\nOut of luck. ");
            writeAll(STDOUT_FILENO, errnoText(errno, errorTextBuffer));
            writeAll(STDOUT_FILENO, "\r\n\n");
            ::_exit(EXIT_FAILURE);
            break;
        }
    }
}

Process::~Process()
{
    if (_d->pid != -1)
        (void) wait();
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
    {
        auto const _ = lock_guard { exitStatusMutex };
        if (exitStatus.has_value())
            return exitStatus;
    }

    assert(pid != -1);
    int status = 0;
    int const rv = waitpid(pid, &status, waitForExit ? 0 : WNOHANG);

    if (rv < 0)
    {
        auto const waitPidErrorCode = errno;
        auto const _ = lock_guard { exitStatusMutex };
        if (exitStatus.has_value())
            return exitStatus;
        errorLog()("waitpid() failed: {}", std::generic_category().message(waitPidErrorCode));
        return std::nullopt;
    }
    else if (rv == 0 && !waitForExit)
        return nullopt;
    else
    {
        auto const _ = lock_guard { exitStatusMutex };
        pid = -1;

        if (WIFEXITED(status))
            return exitStatus = ExitStatus { NormalExit { WEXITSTATUS(status) } };
        else if (WIFSIGNALED(status))
            return exitStatus = ExitStatus { SignalExit { WTERMSIG(status) } };
        else if (WIFSTOPPED(status))
            return exitStatus = ExitStatus { SignalExit { SIGSTOP } };
        else
            // TODO: handle the other WIF....(status) cases.
            throw runtime_error { "Unknown waitpid() return value." };
    }
}

void Process::terminate(TerminationHint terminationHint)
{
    if (!alive())
        return;

    ::kill(_d->pid, terminationHint == TerminationHint::Hangup ? SIGHUP : SIGTERM);
}

Process::ExitStatus Process::wait()
{
    return *_d->checkStatus(true);
}

namespace
{
    /// Resolves the user's login shell, asking flatpak's host side when sandbox escape is requested.
    ///
    /// @param escapeSandbox Whether the shell is to be run outside the flatpak sandbox.
    /// @return argv of the login shell; `{"/bin/sh"}` if the user has no password-database entry.
    [[nodiscard]] vector<string> resolveLoginShell(bool escapeSandbox)
    {
        if (auto const pw = crispy::currentUserPasswordEntry(); pw.has_value())
        {
#ifdef __APPLE__
            crispy::ignore_unused(escapeSandbox);
            return { pw->shell };
#else
            if (Process::isFlatpak() && escapeSandbox)
            {
                char buf[1024];
                auto const cmd = std::format("flatpak-spawn --host getent passwd {}", pw->name);
                FILE* fp = popen(cmd.c_str(), "r");
                auto fpCloser = crispy::finally { [fp]() { pclose(fp); } };
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
    if (auto const pw = crispy::currentUserPasswordEntry(); pw.has_value() && !pw->name.empty())
        return pw->name;

    if (auto const user = crispy::environment::get("USER"); user.has_value())
        return std::string { *user };

    return "unknown";
}

fs::path Process::homeDirectory()
{
    if (auto const home = crispy::environment::get("HOME"); home.has_value())
        return { *home };
    else if (auto const pw = crispy::currentUserPasswordEntry(); pw.has_value() && !pw->homeDirectory.empty())
        return { pw->homeDirectory };
    else
        return { "/" };
}

string Process::workingDirectory() const
{
#ifdef __linux__
    try
    {
        auto const path = fs::path { std::format("/proc/{}/cwd", _d->pid) };
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
        auto vpi = proc_vnodepathinfo {};
        auto const pid = tcgetpgrp(unbox<int>(static_cast<UnixPty const*>(_d->pty.get())->handle()));

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

} // namespace vtpty
