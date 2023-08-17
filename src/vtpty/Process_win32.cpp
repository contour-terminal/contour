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
#include <vtpty/ConPty.h>
#include <vtpty/Process.h>
#include <vtpty/Pty.h>

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

        explicit InheritingEnvBlock(Environment const& newValues)
        {
            for (auto const& env: newValues)
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

    HRESULT initializeStartupInfoAttachedToPTY(STARTUPINFOEX& startupInfoEx, ConPty& pty)
    {
        // Initializes the specified startup info struct with the required properties and
        // updates its thread attribute list with the specified ConPTY handle

        HRESULT hr { E_UNEXPECTED };

        size_t attrListSize {};

        startupInfoEx.StartupInfo.cb = sizeof(STARTUPINFOEX);

        // Get the size of the thread attribute list.
        InitializeProcThreadAttributeList(NULL, 1, 0, &attrListSize);

        // Allocate a thread attribute list of the correct size
        startupInfoEx.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(malloc(attrListSize));

        // Initialize thread attribute list
        if (startupInfoEx.lpAttributeList
            && InitializeProcThreadAttributeList(startupInfoEx.lpAttributeList, 1, 0, &attrListSize))
        {
            // Set Pseudo Console attribute
            hr = UpdateProcThreadAttribute(startupInfoEx.lpAttributeList,
                                           0,
                                           PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                           pty.master(),
                                           sizeof(decltype(pty.master())),
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

    char** createArgv(string const& arg0, std::vector<string> const& args, size_t i = 0)
    {
        auto const argCount = args.size(); // factor out in order to avoid false-positive by static analysers.
        char** argv = new char*[argCount + 2 - i];
        argv[0] = const_cast<char*>(arg0.c_str());
        for (size_t i = 0; i < argCount; ++i)
            argv[i + 1] = const_cast<char*>(args[i].c_str());
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

    [[nodiscard]] optional<Process::ExitStatus> checkStatus(bool waitForExit) const;
};

Process::Process(string const& path,
                 vector<string> const& args,
                 FileSystem::path const& cwd,
                 Environment const& env,
                 bool escapeSandbox,
                 std::unique_ptr<Pty> pty):
    _d(new Private { path, args, cwd, env, std::move(pty) }, [](Private* p) { delete p; })
{
    crispy::ignore_unused(escapeSandbox);
}

bool Process::isFlatpak()
{
    return false;
}

void Process::start()
{
    Require(static_cast<ConPty const*>(_d->pty.get()));

    _d->pty->start();

    initializeStartupInfoAttachedToPTY(_d->startupInfo, static_cast<ConPty&>(*_d->pty));

    string cmd = _d->path;
    for (size_t i = 0; i < _d->args.size(); ++i)
    {
        cmd += ' ';
        if (_d->args[i].find(' ') != std::string::npos)
            cmd += '\"' + _d->args[i] + '\"';
        else
            cmd += _d->args[i];
    }

    // In case of PATH environment variable, extend it rather then overwriting it.
    auto env = _d->env;
    for (auto const& [name, value]: _d->env)
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

    auto const cwd = _d->cwd.generic_string();
    auto const cwdPtr = !cwd.empty() ? cwd.c_str() : nullptr;

    ptyLog()("Creating process for command line: {}", cmd);

    BOOL success = CreateProcess(nullptr,                        // No module name - use Command Line
                                 const_cast<LPSTR>(cmd.c_str()), // Command Line
                                 nullptr,                        // Process handle not inheritable
                                 nullptr,                        // Thread handle not inheritable
                                 FALSE,                          // Inherit handles
                                 EXTENDED_STARTUPINFO_PRESENT,   // Creation flags
                                 nullptr,                        // Use parent's environment block
                                 const_cast<LPSTR>(cwdPtr),      // Use parent's starting directory
                                 &_d->startupInfo.StartupInfo,   // Pointer to STARTUPINFO
                                 &_d->processInfo);              // Pointer to PROCESS_INFORMATION

    if (!success)
    {
        success = CreateProcess(nullptr, // No module name - use Command Line
                                const_cast<LPSTR>((this->loginShell(false)[0]).c_str()), // Command Line
                                nullptr,                      // Process handle not inheritable
                                nullptr,                      // Thread handle not inheritable
                                FALSE,                        // Inherit handles
                                EXTENDED_STARTUPINFO_PRESENT, // Creation flags
                                nullptr,                      // Use parent's environment block
                                const_cast<LPSTR>(cwdPtr),    // Use parent's starting directory
                                &_d->startupInfo.StartupInfo, // Pointer to STARTUPINFO
                                &_d->processInfo);            // Pointer to PROCESS_INFORMATION
    }

    if (!success)
    {
        throw runtime_error { "Could not create process. "s + getLastErrorAsString() };
    }

    _d->exitWatcher = std::thread([this]() {
        (void) wait();
        ptyLog()("Process terminated with exit code {}.", checkStatus().value());
        _d->pty->close();
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

Process::~Process()
{
    if (_d->exitWatcher)
        _d->exitWatcher.value().join();

    CloseHandle(_d->processInfo.hThread);
    CloseHandle(_d->processInfo.hProcess);

    DeleteProcThreadAttributeList(_d->startupInfo.lpAttributeList);
    free(_d->startupInfo.lpAttributeList);
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

    if (waitForExit)
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

void Process::terminate(TerminationHint terminationHint)
{
    if (!alive())
        return;

    TerminateProcess(_d->pid, 1);
}

Process::ExitStatus Process::wait()
{
    return *_d->checkStatus(true);
}

vector<string> Process::loginShell(bool escapeSandbox)
{
    crispy::ignore_unused(escapeSandbox);

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
