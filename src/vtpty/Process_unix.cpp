// SPDX-License-Identifier: Apache-2.0
#include <vtpty/Process.h>
#include <vtpty/Pty.h>
#include <vtpty/UnixPty.h>

#include <crispy/overloaded.h>
#include <crispy/utils.h>

#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <mutex>
#include <stdexcept>
#include <string>

#if !defined(__FreeBSD__)
    #include <utmp.h>
#endif

#if defined(__APPLE__)
    #include <libproc.h>
    #include <util.h>
#endif

#if defined(__linux__)
    #include <pty.h>
#endif

#include <sys/types.h>
#include <sys/wait.h>

#include <csignal>

#include <pwd.h>
#include <unistd.h>

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
        return strerror(errno);
    }

    [[nodiscard]] char** createArgv(string const& arg0, vector<string> const& args, size_t startIndex = 0)
    {
        // Factor out in order to avoid false-positive by static analysers.
        auto const argCount = args.size() - startIndex;
        assert(startIndex <= args.size());

        char** argv = new char*[argCount + 2];
        argv[0] = strdup(arg0.c_str());
        for (size_t i = 0; i < argCount; ++i)
            argv[i + 1] = strdup(args[i + startIndex].c_str());
        argv[argCount + 1] = nullptr;
        return argv;
    }

    void saveDup2(int a, int b)
    {
        while (dup2(a, b) == -1 && (errno == EBUSY || errno == EINTR))
            ;
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

    _d->pid = fork();

    UnixPipe* stdoutFastPipe = [this]() -> UnixPipe* {
        if (auto* p = dynamic_cast<UnixPty*>(_d->pty.get()))
            return &p->stdoutFastPipe();
        return nullptr;
    }();

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
            if (!isFlatpak() || !_d->escapeSandbox)
            {
                if (!_d->cwd.empty() && chdir(cwd.c_str()) != 0)
                {
                    printf("Failed to chdir to \"%s\". %s\n", cwd.c_str(), strerror(errno));
                    exit(EXIT_FAILURE);
                }

                if (isFlatpak() && !_d->escapeSandbox)
                    setenv("TERMINFO", "/app/share/terminfo", true);

                for (auto&& [name, value]: _d->env)
                    setenv(name.c_str(), value.c_str(), true);

                if (stdoutFastPipe)
                    setenv(StdoutFastPipeEnvironmentName.data(), StdoutFastPipeFdStr.data(), true);
            }

            char** argv = [stdoutFastPipe, this]() -> char** {
                if (!isFlatpak() || !_d->escapeSandbox)
                    return createArgv(_d->path, _d->args, 0);

                auto const terminfoBaseDirectory =
                    homeDirectory() / ".var/app/org.contourterminal.Contour/terminfo";

                // Prepend flatpak to jump out of sandbox:
                // flatpak-spawn --host --watch-bus --env=TERM=$TERM /bin/zsh
                auto realArgs = vector<string> {};
                realArgs.emplace_back("--host");
                realArgs.emplace_back("--watch-bus");
                realArgs.emplace_back(
                    std::format("--env=TERMINFO={}", terminfoBaseDirectory.generic_string()));
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

                return createArgv("/usr/bin/flatpak-spawn", realArgs, 0);
            }();

            if (auto* pty = dynamic_cast<UnixPty*>(_d->pty.get()))
            {
                if (pty->stdoutFastPipe().writer() != -1)
                {
                    saveDup2(pty->stdoutFastPipe().writer(), StdoutFastPipeFd);
                    pty->stdoutFastPipe().close();
                }
            }

            // maybe close any leaked/inherited file descriptors from parent process
            // TODO: But be a little bit more clever in iterating only over those that are actually still
            // open.
            for (int i = StdoutFastPipeFd + 1; i < 256; ++i)
                ::close(i);

            // reset signal(s) to default that may have been changed in the parent process.
            signal(SIGPIPE, SIG_DFL);

            ::execvp(argv[0], argv);

            // Fallback: Try login shell.
            auto theLoginShell = loginShell(_d->escapeSandbox);
            fprintf(stdout,
                    "\r\033[31;1mFailed to spawn %s\033[m\r\nTrying login shell: %s\n",
                    argv[0],
                    crispy::joinHumanReadableQuoted(theLoginShell, ' ').c_str());
            fflush(stdout);
            if (!theLoginShell.empty())
            {
                delete[] argv;
                argv = createArgv(theLoginShell[0], theLoginShell, 1);
                ::execvp(argv[0], argv);
            }

            // Bad luck.
            fprintf(stdout, "\r\nOut of luck. %s\r\n\n", strerror(errno));
            fflush(stdout);
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
        errorLog()("waitpid() failed: {}", strerror(waitPidErrorCode));
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

vector<string> Process::loginShell(bool escapeSandbox)
{
    if (passwd const* pw = getpwuid(getuid()); pw != nullptr)
    {
#if defined(__APPLE__)
        crispy::ignore_unused(escapeSandbox);
        return { pw->pw_shell };
#else
        if (isFlatpak() && escapeSandbox)
        {
            char buf[1024];
            auto const cmd = std::format("flatpak-spawn --host getent passwd {}", pw->pw_name);
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

        return { pw->pw_shell };
#endif
    }
    else
        return { "/bin/sh"s };
}

std::string Process::userName()
{
    if (passwd const* pw = getpwuid(getuid()); pw != nullptr)
        return pw->pw_name;

    if (char const* user = getenv("USER"); user != nullptr)
        return user;

    return "unknown";
}

fs::path Process::homeDirectory()
{
    if (auto const* home = getenv("HOME"); home != nullptr)
        return fs::path(home);
    else if (passwd const* pw = getpwuid(getuid()); pw != nullptr)
        return fs::path(pw->pw_dir);
    else
        return fs::path("/");
}

string Process::workingDirectory() const
{
#if defined(__linux__)
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
#elif defined(__APPLE__)
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
