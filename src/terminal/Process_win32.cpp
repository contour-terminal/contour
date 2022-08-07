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
#include <terminal/Process.h>
#include <terminal/pty/ConPty.h>
#include <terminal/pty/Pty.h>

#include <crispy/assert.h>
#include <crispy/overloaded.h>
#include <crispy/stdfs.h>

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

#include <Windows.h>
#include <direct.h>
#include <errno.h>

using namespace std;

namespace terminal
{

namespace
{
    string getLastErrorAsString()
    {
        DWORD errorMessageID = GetLastError();
        if (errorMessageID == 0)
            return "";

        LPSTR messageBuffer = nullptr;
        size_t size = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
                                         | FORMAT_MESSAGE_IGNORE_INSERTS,
                                     nullptr,
                                     errorMessageID,
                                     MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                     (LPSTR) &messageBuffer,
                                     0,
                                     nullptr);

        string message(messageBuffer, size);

        LocalFree(messageBuffer);

        return message;
    }

    class InheritingEnvBlock
    {
      public:
        using Environment = terminal::Process::Environment;

        explicit InheritingEnvBlock(Environment const& _newValues)
        {
            for (auto const& env: _newValues)
            {
                if (auto len = GetEnvironmentVariable(env.first.c_str(), nullptr, 0); len != 0)
                {
                    vector<char> buf;
                    buf.resize(len);
                    GetEnvironmentVariable(env.first.c_str(), &buf[0], len);
                    oldValues_[env.first] = string(&buf[0], len - 1);
                }
                if (!env.second.empty())
                    SetEnvironmentVariable(env.first.c_str(), env.second.c_str());
                else
                    SetEnvironmentVariable(env.first.c_str(), nullptr);
            }
        }

        ~InheritingEnvBlock()
        {
            for (auto const& env: oldValues_)
                SetEnvironmentVariable(env.first.c_str(), env.second.c_str());
        }

      private:
        Environment oldValues_;
    };

    HRESULT initializeStartupInfoAttachedToPTY(STARTUPINFOEX& _startupInfoEx, ConPty& _pty)
    {
        // Initializes the specified startup info struct with the required properties and
        // updates its thread attribute list with the specified ConPTY handle

        HRESULT hr { E_UNEXPECTED };

        size_t attrListSize {};

        _startupInfoEx.StartupInfo.cb = sizeof(STARTUPINFOEX);

        // Get the size of the thread attribute list.
        InitializeProcThreadAttributeList(NULL, 1, 0, &attrListSize);

        // Allocate a thread attribute list of the correct size
        _startupInfoEx.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(malloc(attrListSize));

        // Initialize thread attribute list
        if (_startupInfoEx.lpAttributeList
            && InitializeProcThreadAttributeList(_startupInfoEx.lpAttributeList, 1, 0, &attrListSize))
        {
            // Set Pseudo Console attribute
            hr = UpdateProcThreadAttribute(_startupInfoEx.lpAttributeList,
                                           0,
                                           PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                           _pty.master(),
                                           sizeof(decltype(_pty.master())),
                                           nullptr,
                                           nullptr)
                     ? S_OK
                     : HRESULT_FROM_WIN32(GetLastError());
        }
        else
        {
            hr = HRESULT_FROM_WIN32(GetLastError());
        }
        return hr;
    }

    char** createArgv(string const& _arg0, std::vector<string> const& _args, size_t i = 0)
    {
        auto const argCount =
            _args.size(); // factor out in order to avoid false-positive by static analysers.
        char** argv = new char*[argCount + 2 - i];
        argv[0] = const_cast<char*>(_arg0.c_str());
        for (size_t i = 0; i < argCount; ++i)
            argv[i + 1] = const_cast<char*>(_args[i].c_str());
        argv[argCount + 1] = nullptr;
        return argv;
    }
} // anonymous namespace

struct Process::Private
{
    string path;
    vector<string> args;
    FileSystem::path cwd;
    Environment env;
    std::unique_ptr<Pty> pty {};

    mutable HANDLE pid {};
    mutable std::mutex exitStatusMutex {};
    mutable std::optional<Process::ExitStatus> exitStatus {};
    std::optional<std::thread> exitWatcher;

    PROCESS_INFORMATION processInfo {};
    STARTUPINFOEX startupInfo {};

    [[nodiscard]] optional<Process::ExitStatus> checkStatus(bool _waitForExit) const;
};

Process::Process(string const& _path,
                 vector<string> const& _args,
                 FileSystem::path const& _cwd,
                 Environment const& _env,
                 std::unique_ptr<Pty> _pty):
    d(new Private {_path, _args, _cwd, _env, move(_pty)}, [](Private* p) { delete p; })
{
}

void Process::start()
{
    Require(static_cast<ConPty const*>(d->pty.get()));

    d->pty->start();

    initializeStartupInfoAttachedToPTY(d->startupInfo, static_cast<ConPty&>(*d->pty));

    string cmd = d->path;
    for (size_t i = 0; i < d->args.size(); ++i)
    {
        cmd += ' ';
        if (d->args[i].find(' ') != std::string::npos)
            cmd += '\"' + d->args[i] + '\"';
        else
            cmd += d->args[i];
    }

    // In case of PATH environment variable, extend it rather then overwriting it.
    auto env = d->env;
    for (auto const& [name, value]: d->env)
    {
        if (crispy::toUpper(name) == "PATH")
        {
            char buf[1024];
            size_t len = 0;
            if (getenv_s(&len, buf, sizeof(buf), "PATH") == 0)
                env[name] = fmt::format("{};{}", value, buf);
        }
    }
    auto const envScope = InheritingEnvBlock { env };

    auto const cwd = d->cwd.generic_string();
    auto const cwdPtr = !cwd.empty() ? cwd.c_str() : nullptr;

    PtyLog()("Creating process for command line: {}", cmd);

    BOOL success = CreateProcess(nullptr,                        // No module name - use Command Line
                                 const_cast<LPSTR>(cmd.c_str()), // Command Line
                                 nullptr,                        // Process handle not inheritable
                                 nullptr,                        // Thread handle not inheritable
                                 FALSE,                          // Inherit handles
                                 EXTENDED_STARTUPINFO_PRESENT,   // Creation flags
                                 nullptr,                        // Use parent's environment block
                                 const_cast<LPSTR>(cwdPtr),      // Use parent's starting directory
                                 &d->startupInfo.StartupInfo,    // Pointer to STARTUPINFO
                                 &d->processInfo);               // Pointer to PROCESS_INFORMATION
    if (!success)
        throw runtime_error { "Could not create process. "s + getLastErrorAsString() };

    d->exitWatcher = std::thread([this]() {
        (void) wait();
        PtyLog()("Process terminated with exit code {}.", checkStatus().value());
        d->pty->close();
    });
}

Pty& Process::pty() noexcept
{
    return *d->pty;
}

Pty const& Process::pty() const noexcept
{
    return *d->pty;
}

Process::~Process()
{
    if (d->exitWatcher)
        d->exitWatcher.value().join();

    CloseHandle(d->processInfo.hThread);
    CloseHandle(d->processInfo.hProcess);

    DeleteProcThreadAttributeList(d->startupInfo.lpAttributeList);
    free(d->startupInfo.lpAttributeList);
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

    if (_waitForExit)
        if (WaitForSingleObject(processInfo.hThread, INFINITE /*10 * 1000*/) != S_OK)
            printf("WaitForSingleObject(thr): %s\n", getLastErrorAsString().c_str());

    DWORD exitCode;
    if (!GetExitCodeProcess(processInfo.hProcess, &exitCode))
        throw runtime_error { getLastErrorAsString() };
    else if (exitCode == STILL_ACTIVE)
        return exitStatus;
    else
        return exitStatus = ExitStatus { NormalExit { static_cast<int>(exitCode) } };
}

void Process::terminate(TerminationHint _terminationHint)
{
    if (!alive())
        return;

    TerminateProcess(d->pid, 1);
}

Process::ExitStatus Process::wait()
{
    return *d->checkStatus(true);
}

vector<string> Process::loginShell()
{
    return { "powershell.exe"s }; // TODO: Find out what the user's default shell is.
}

FileSystem::path Process::homeDirectory()
{
    if (char const* p = getenv("USERPROFILE"); p && *p)
        return FileSystem::path(p);

    return FileSystem::path("/");
}

string Process::workingDirectory() const
{
    // TODO: Windows
    return "."s;
}

} // namespace terminal
