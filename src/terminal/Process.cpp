/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019 Christian Parpart <christian@parpart.family>
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
#include <terminal/PseudoTerminal.h>

#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>

#if !defined(_MSC_VER)
#include <utmp.h>
#include <pwd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace std;

namespace {
    string getLastErrorAsString()
    {
#if defined(__unix__)
        return strerror(errno);
#else
        DWORD errorMessageID = GetLastError();
        if (errorMessageID == 0)
            return "";

        LPSTR messageBuffer = nullptr;
        size_t size = FormatMessageA(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr,
            errorMessageID,
            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            (LPSTR)& messageBuffer,
            0,
            nullptr
        );

        string message(messageBuffer, size);

        LocalFree(messageBuffer);

        return message;
#endif
    }
} // anonymous namespace

namespace terminal {

#if defined(_MSC_VER)
namespace {
    HRESULT initializeStartupInfoAttachedToPTY(STARTUPINFOEX& _startupInfoEx, PseudoTerminal& _pty)
    {
        // Initializes the specified startup info struct with the required properties and
        // updates its thread attribute list with the specified ConPTY handle

        HRESULT hr{ E_UNEXPECTED };

        size_t attrListSize{};

        _startupInfoEx.StartupInfo.cb = sizeof(STARTUPINFOEX);

        // Get the size of the thread attribute list.
        InitializeProcThreadAttributeList(NULL, 1, 0, &attrListSize);

        // Allocate a thread attribute list of the correct size
        _startupInfoEx.lpAttributeList =
            reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(malloc(attrListSize));

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
} // anonymous namespace
#endif

Process::Process(
    PseudoTerminal& _pty,
    string const& _path,
    vector<string> const& _args,
    Environment const& _env)
{
#if defined(__unix__)
    pid_ = fork();
    switch (pid_)
    {
        default: // in parent
            close(_pty.slave()); // TODO: release slave in PTY object
            break;
        case -1: // fork error
            throw runtime_error{ getLastErrorAsString() };
        case 0:  // in child
        {
            close(_pty.master());
            if (login_tty(_pty.slave()) < 0)
                _exit(EXIT_FAILURE);

            char** argv = new char* [_args.size() + 1];
            for (size_t i = 0; i < _args.size(); ++i)
                argv[i] = const_cast<char*>(_args[i].c_str());
			argv[_args.size()] = nullptr;

            for (auto&& [name, value] : _env)
                setenv(name.c_str(), value.c_str(), true);

            ::execvp(_path.c_str(), argv);
            ::_exit(EXIT_FAILURE);
            break;
        }
    }
#else
    initializeStartupInfoAttachedToPTY(startupInfo_, _pty);

    string cmd = _path;
    for (size_t i = 1; i < _args.size(); ++i)
        cmd += " " + _args[i];

    // TODO: _env

    BOOL success = CreateProcess(
        nullptr,                            // No module name - use Command Line
        const_cast<LPSTR>(cmd.c_str()),     // Command Line
        nullptr,                            // Process handle not inheritable
        nullptr,                            // Thread handle not inheritable
        FALSE,                              // Inherit handles
        EXTENDED_STARTUPINFO_PRESENT,       // Creation flags
        nullptr,                            // Use parent's environment block
        nullptr,                            // Use parent's starting directory
        &startupInfo_.StartupInfo,          // Pointer to STARTUPINFO
        &processInfo_);                     // Pointer to PROCESS_INFORMATION
    if (!success)
        throw runtime_error{ getLastErrorAsString() };
#endif
}

Process::~Process()
{
#if defined(__unix__)
    if (pid_ != -1)
        (void) wait();
#else
    CloseHandle(processInfo_.hThread);
    CloseHandle(processInfo_.hProcess);
 
    DeleteProcThreadAttributeList(startupInfo_.lpAttributeList);
    free(startupInfo_.lpAttributeList);
#endif
}

optional<Process::ExitStatus> Process::checkStatus() const
{
    if (exitStatus_.has_value())
        return exitStatus_;

#if defined(__unix__)
    assert(pid_ != -1);
    int status = 0;
    if (waitpid(pid_, &status, WNOHANG) == -1)
        throw runtime_error{ getLastErrorAsString() };

    pid_ = -1;

    if (WIFEXITED(status))
        return exitStatus_ = ExitStatus{ NormalExit{ WEXITSTATUS(status) } };
    else if (WIFSIGNALED(status))
        return exitStatus_ = ExitStatus{ SignalExit{ WTERMSIG(status) } };
    else if (WIFSTOPPED(status))
        return exitStatus_ = ExitStatus{ Suspend{} };
    else if (WIFCONTINUED(status))
        return exitStatus_ = ExitStatus{ Resume{} };
    else
        throw runtime_error{ "Unknown waitpid() return value." };
#else
    if (WaitForSingleObject(processInfo_.hThread, 0) != S_OK)
        printf("WaitForSingleObject(thr): %s\n", getLastErrorAsString().c_str());
    DWORD exitCode;
    if (GetExitCodeProcess(processInfo_.hProcess, &exitCode))
        return exitStatus_ = ExitStatus{ NormalExit{ static_cast<int>(exitCode) } };
    else
        throw runtime_error{ getLastErrorAsString() };
#endif
}

Process::ExitStatus Process::wait()
{
    if (exitStatus_.has_value())
        return exitStatus_.value();

#if defined(__unix__)
    assert(pid_ != -1);
    int status = 0;
    if (waitpid(pid_, &status, 0) == -1)
        throw runtime_error{getLastErrorAsString()};

    pid_ = -1;

    if (WIFEXITED(status))
        return exitStatus_ = ExitStatus{ NormalExit{ WEXITSTATUS(status) } };
    else if (WIFSIGNALED(status))
        return exitStatus_ = ExitStatus{ SignalExit{ WTERMSIG(status) } };
    else if (WIFSTOPPED(status))
        return exitStatus_ = ExitStatus{ Suspend{} };
    else if (WIFCONTINUED(status))
        return exitStatus_ = ExitStatus{ Resume{} };
    else
        throw runtime_error{ "Unknown waitpid() return value." };
#else
    if (WaitForSingleObject(processInfo_.hThread, INFINITE /*10 * 1000*/) != S_OK)
        printf("WaitForSingleObject(thr): %s\n", getLastErrorAsString().c_str());
    //if (WaitForSingleObject(processInfo_.hProcess, INFINITE) != S_OK)
    //    printf("WaitForSingleObject(proc): %s\n", getLastErrorAsString().c_str());
    DWORD exitCode;
    if (GetExitCodeProcess(processInfo_.hProcess, &exitCode))
        return *(exitStatus_ = ExitStatus{ NormalExit{ static_cast<int>(exitCode) } });
    else
        throw runtime_error{ getLastErrorAsString() };
#endif
}

std::string Process::loginShell()
{
#if defined(_MSC_VER)
    return "powershell.exe"s;
#else
    if (passwd const* pw = getpwuid(getuid()); pw != nullptr)
        return pw->pw_shell;
    else
        return "/bin/sh"s;
#endif
}

}  // namespace terminal
