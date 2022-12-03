/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <terminal/pty/Process.h>
#include <terminal/pty/Pty.h>
#include <terminal/pty/UnixPty.h>

#if defined(__linux__)
    #include <terminal/pty/LinuxPty.h>
#endif

#include <crispy/overloaded.h>
#include <crispy/stdfs.h>
#include <crispy/utils.h>

#include <fmt/format.h>

#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <string>
#include <unordered_map>

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

namespace terminal
{

#if defined(__linux__)
using SystemPty = LinuxPty;
#else
using SystemPty = UnixPty;
#endif

namespace
{
    constexpr auto StdoutFastPipeFd = 3;
    constexpr auto StdoutFastPipeFdStr = "3"sv;
    constexpr auto StdoutFastPipeEnvironmentName = "STDOUT_FASTPIPE"sv;

    string getLastErrorAsString()
    {
        return strerror(errno);
    }

    [[nodiscard]] char** createArgv(string const& _arg0,
                                    std::vector<string> const& _args,
                                    size_t startIndex = 0)
    {
        // Factor out in order to avoid false-positive by static analysers.
        auto const argCount = _args.size() - startIndex;
        assert(startIndex <= _args.size());

        char** argv = new char*[argCount + 2];
        argv[0] = strdup(_arg0.c_str());
        for (size_t i = 0; i < argCount; ++i)
            argv[i + 1] = strdup(_args[i + startIndex].c_str());
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
    FileSystem::path cwd;
    Environment env;
    bool escapeSandbox;

    unique_ptr<Pty> pty {};
    mutable pid_t pid {};
    mutable std::mutex exitStatusMutex {};
    mutable std::optional<Process::ExitStatus> exitStatus {};

    [[nodiscard]] std::optional<ExitStatus> checkStatus(bool _waitForExit) const;
};

Process::Process(string const& _path,
                 vector<string> const& _args,
                 FileSystem::path const& _cwd,
                 Environment const& _env,
                 bool escapeSandbox,
                 unique_ptr<Pty> _pty):
    d(new Private { _path, _args, _cwd, _env, escapeSandbox, std::move(_pty) }, [](Private* p) { delete p; })
{
}

bool Process::isFlatpak()
{
    static bool check = FileSystem::exists("/.flatpak-info");
    return check;
}

void Process::start()
{
    d->pty->start();

    d->pid = fork();

    UnixPipe* stdoutFastPipe = [this]() -> UnixPipe* {
        if (auto* p = dynamic_cast<SystemPty*>(d->pty.get()))
            return &p->stdoutFastPipe();
        return nullptr;
    }();

    switch (d->pid)
    {
        default: // in parent
            d->pty->slave().close();
            if (stdoutFastPipe)
                stdoutFastPipe->closeWriter();
            break;
        case -1: // fork error
            throw runtime_error { getLastErrorAsString() };
        case 0: // in child
        {
            (void) d->pty->slave().login();

            auto const& cwd = d->cwd.generic_string();
            if (!isFlatpak() || !d->escapeSandbox)
            {
                if (!d->cwd.empty() && chdir(cwd.c_str()) != 0)
                {
                    printf("Failed to chdir to \"%s\". %s\n", cwd.c_str(), strerror(errno));
                    exit(EXIT_FAILURE);
                }

                if (isFlatpak() && !d->escapeSandbox)
                    setenv("TERMINFO", "/app/share/terminfo", true);

                for (auto&& [name, value]: d->env)
                    setenv(name.c_str(), value.c_str(), true);

                if (stdoutFastPipe)
                    setenv(StdoutFastPipeEnvironmentName.data(), StdoutFastPipeFdStr.data(), true);
            }

            char** argv = [stdoutFastPipe, this]() -> char** {
                if (!isFlatpak() || !d->escapeSandbox)
                    return createArgv(d->path, d->args, 0);

                auto const terminfoBaseDirectory =
                    homeDirectory() / ".var/app/org.contourterminal.Contour/terminfo";

                // Prepend flatpak to jump out of sandbox:
                // flatpak-spawn --host --watch-bus --env=TERM=$TERM /bin/zsh
                auto realArgs = std::vector<string> {};
                realArgs.emplace_back("--host");
                realArgs.emplace_back("--watch-bus");
                realArgs.emplace_back(
                    fmt::format("--env=TERMINFO={}", terminfoBaseDirectory.generic_string()));
                if (stdoutFastPipe)
                {
                    realArgs.emplace_back(
                        fmt::format("--env={}={}", StdoutFastPipeEnvironmentName, StdoutFastPipeFdStr));
                    realArgs.emplace_back(fmt::format("--forward-fd={}", StdoutFastPipeFdStr));
                }
                if (!d->cwd.empty())
                    realArgs.emplace_back(fmt::format("--directory={}", d->cwd.generic_string()));
                realArgs.emplace_back(fmt::format("--env=TERM={}", "contour"));
                for (auto&& [name, value]: d->env)
                    realArgs.emplace_back(fmt::format("--env={}={}", name, value));
                if (stdoutFastPipe)
                    realArgs.emplace_back(
                        fmt::format("--env={}={}", StdoutFastPipeEnvironmentName, StdoutFastPipeFd));
                realArgs.push_back(d->path);
                for (auto const& arg: d->args)
                    realArgs.push_back(arg);

                return createArgv("/usr/bin/flatpak-spawn", realArgs, 0);
            }();

            if (auto pty = dynamic_cast<SystemPty*>(d->pty.get()))
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
            auto theLoginShell = loginShell(d->escapeSandbox);
            fprintf(stdout,
                    "\r\n\033[31;1mFailed to spawn \"%s\". %s\033[m\r\nTrying login shell: %s\n",
                    argv[0],
                    strerror(errno),
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
    if (d->pid != -1)
        (void) wait();
}

Pty& Process::pty() noexcept
{
    return *d->pty;
}

Pty const& Process::pty() const noexcept
{
    return *d->pty;
}

optional<Process::ExitStatus> Process::checkStatus() const
{
    return d->checkStatus(false);
}

optional<Process::ExitStatus> Process::Private::checkStatus(bool _waitForExit) const
{
    {
        auto const _ = lock_guard { exitStatusMutex };
        if (exitStatus.has_value())
            return exitStatus;
    }

    assert(pid != -1);
    int status = 0;
    int const rv = waitpid(pid, &status, _waitForExit ? 0 : WNOHANG);

    if (rv < 0)
    {
        auto const _ = lock_guard { exitStatusMutex };
        if (exitStatus.has_value())
            return exitStatus;
        throw runtime_error { "waitpid: "s + getLastErrorAsString() };
    }
    else if (rv == 0 && !_waitForExit)
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

void Process::terminate(TerminationHint _terminationHint)
{
    if (!alive())
        return;

    ::kill(d->pid, _terminationHint == TerminationHint::Hangup ? SIGHUP : SIGTERM);
}

Process::ExitStatus Process::wait()
{
    return *d->checkStatus(true);
}

vector<string> Process::loginShell(bool escapeSandbox)
{
    if (passwd const* pw = getpwuid(getuid()); pw != nullptr)
    {
#if defined(__APPLE__)
        crispy::ignore_unused(escapeSandbox);
        auto shell = string(pw->pw_shell);
        auto index = shell.rfind('/');
        return { "/bin/bash", "-c", fmt::format("exec -a -{} {}", shell.substr(index + 1, 5), pw->pw_shell) };
#else
        if (isFlatpak() && escapeSandbox)
        {
            char buf[1024];
            auto const cmd = fmt::format("flatpak-spawn --host getent passwd {}", pw->pw_name);
            FILE* fp = popen(cmd.c_str(), "r");
            auto fpCloser = crispy::finally { [fp]() {
                pclose(fp);
            } };
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

FileSystem::path Process::homeDirectory()
{
    if (passwd const* pw = getpwuid(getuid()); pw != nullptr)
        return FileSystem::path(pw->pw_dir);
    else
        return FileSystem::path("/");
}

string Process::workingDirectory() const
{
#if defined(__linux__)
    try
    {
        auto const path = FileSystem::path { fmt::format("/proc/{}/cwd", d->pid) };
        auto const cwd = FileSystem::read_symlink(path);
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
        auto const pid = tcgetpgrp(unbox<int>(static_cast<SystemPty const*>(d->pty.get())->handle()));

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

} // namespace terminal
