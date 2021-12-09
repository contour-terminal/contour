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
#include <terminal/pty/Pty.h>
#include <crispy/stdfs.h>
#include <crispy/overloaded.h>
#include <fmt/format.h>

#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <unordered_map>

#if !defined(_WIN32)
#if !defined(__FreeBSD__)
#include <utmp.h>
#endif
#include <pwd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#else
#include <direct.h>
#include <errno.h>
#endif

#if defined(_MSC_VER)
#include <terminal/pty/ConPty.h>
#endif

using namespace std;

namespace terminal {

namespace {
    string getLastErrorAsString()
    {
		#if defined(__unix__) || defined(__APPLE__)
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

	#if defined(_WIN32)
	class InheritingEnvBlock {
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
	#endif

	#if defined(_WIN32)
    HRESULT initializeStartupInfoAttachedToPTY(STARTUPINFOEX& _startupInfoEx, ConPty& _pty)
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
	#endif
} // anonymous namespace


char** createArgv(string const& _arg0, std::vector<string> const& _args, size_t i = 0)
{
    auto const argCount = _args.size(); // factor out in order to avoid false-positive by static analysers.
    char** argv = new char* [argCount + 2 - i];
    argv[0] = const_cast<char*>(_arg0.c_str());
    for (size_t i = 0; i < argCount; ++i)
        argv[i + 1] = const_cast<char*>(_args[i].c_str());
    argv[argCount + 1] = nullptr;
    return argv;
}

Process::Process(string const& _path,
                 vector<string> const& _args,
                 FileSystem::path const& _cwd,
                 Environment const& _env,
                 Pty& _pty)
{
#if defined(__unix__) || defined(__APPLE__)
    pid_ = fork();
    switch (pid_)
    {
        default: // in parent
            _pty.prepareParentProcess();
            break;
        case -1: // fork error
            throw runtime_error{ getLastErrorAsString() };
        case 0:  // in child
        {
            _pty.prepareChildProcess();

            setsid();

            auto const& cwd = _cwd.generic_string();
            if (!_cwd.empty() && chdir(cwd.c_str()) != 0)
            {
                printf("Failed to chdir to \"%s\". %s\n", cwd.c_str(), strerror(errno));
                exit(EXIT_FAILURE);
            }

            char** argv = createArgv(_path, _args, 0);

            for (auto&& [name, value] : _env)
                setenv(name.c_str(), value.c_str(), true);

            // maybe close any leaked/inherited file descriptors from parent process
            // TODO: But be a little bit more clever in iterating only over those that are actually still open.
            for (int i = 3; i < 256; ++i)
                ::close(i);

            // reset signal(s) to default that may have been changed in the parent process.
            signal(SIGPIPE, SIG_DFL);

            ::execvp(argv[0], argv);

            // Fallback: Try login shell.
            fprintf(stdout, "\r\n\e[31;1mFailed to spawn %s. %s\e[m\r\n\n", argv[0], strerror(errno));
            fflush(stdout);
            auto theLoginShell = loginShell();
            if (!theLoginShell.empty())
            {
                delete[] argv;
                argv = createArgv(_args[0], _args, 1);
                ::execvp(argv[0], argv);
            }

            // Bad luck.
            ::_exit(EXIT_FAILURE);
            break;
        }
    }
#else
    initializeStartupInfoAttachedToPTY(startupInfo_, static_cast<ConPty&>(_pty));

    string cmd = _path;
    for (size_t i = 0; i < _args.size(); ++i)
    {
        cmd += ' ';
        if (_args[i].find(' ') != std::string::npos)
            cmd += '\"' + _args[i] + '\"';
        else
            cmd += _args[i];
    }

	auto const envScope = InheritingEnvBlock{_env};

    auto const cwd = _cwd.generic_string();
    auto const cwdPtr = !cwd.empty() ? cwd.c_str() : nullptr;

    BOOL success = CreateProcess(
        nullptr,                            // No module name - use Command Line
        const_cast<LPSTR>(cmd.c_str()),     // Command Line
        nullptr,                            // Process handle not inheritable
        nullptr,                            // Thread handle not inheritable
        FALSE,                              // Inherit handles
        EXTENDED_STARTUPINFO_PRESENT,       // Creation flags
        nullptr,                            // Use parent's environment block
        const_cast<LPSTR>(cwdPtr),          // Use parent's starting directory
        &startupInfo_.StartupInfo,          // Pointer to STARTUPINFO
        &processInfo_);                     // Pointer to PROCESS_INFORMATION
    if (!success)
        throw runtime_error{ getLastErrorAsString() };
#endif
}

Process::Process(
    string const& _path,
    vector<string> const& _args,
    FileSystem::path const& _cwd,
    Environment const& _env,
	bool _detached)
{
	detached_ = _detached;

#if defined(__unix__) || defined(__APPLE__)
    pid_ = fork();
    switch (pid_)
    {
        default: // in parent
            break;
        case -1: // fork error
            throw runtime_error{ getLastErrorAsString() };
        case 0:  // in child
        {
			if (_detached)
				setsid();

            auto const cwd = _cwd.generic_string();
            if (!_cwd.empty() && chdir(cwd.c_str()) < 0)
			{
				printf("Failed to chdir to \"%s\". %s\n", cwd.c_str(), strerror(errno));
				exit(EXIT_FAILURE);
			}

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
	// TODO: anything to handle wrt. detached spawn?

    string cmd = _path;
    for (size_t i = 1; i < _args.size(); ++i)
    {
        cmd += ' ';
        if (_args[i].find(' ') != std::string::npos)
            cmd += '\"' + _args[i] + '\"';
        else
            cmd += _args[i];
    }

	auto const envScope = InheritingEnvBlock{_env};

    auto const cwd = _cwd.generic_string();
    auto const cwdPtr = !cwd.empty() ? cwd.c_str() : nullptr;

    BOOL success = CreateProcess(
        nullptr,                            // No module name - use Command Line
        const_cast<LPSTR>(cmd.c_str()),     // Command Line
        nullptr,                            // Process handle not inheritable
        nullptr,                            // Thread handle not inheritable
        FALSE,                              // Inherit handles
        EXTENDED_STARTUPINFO_PRESENT,       // Creation flags
        nullptr,                            // Use parent's environment block
        const_cast<LPSTR>(cwdPtr),          // Use parent's starting directory
        &startupInfo_.StartupInfo,          // Pointer to STARTUPINFO
        &processInfo_);                     // Pointer to PROCESS_INFORMATION
    if (!success)
        throw runtime_error{ getLastErrorAsString() };
#endif
}

Process::~Process()
{
#if defined(__unix__) || defined(__APPLE__)
    if (pid_ != -1 && !detached_)
        (void) wait();
#else
    CloseHandle(processInfo_.hThread);
    CloseHandle(processInfo_.hProcess);

    DeleteProcThreadAttributeList(startupInfo_.lpAttributeList);
    free(startupInfo_.lpAttributeList);
#endif
}

bool Process::alive() const noexcept
{
    (void) checkStatus();
    return !exitStatus_.has_value() || !(holds_alternative<NormalExit>(*exitStatus_) ||
                                         holds_alternative<SignalExit>(*exitStatus_));
}

optional<Process::ExitStatus> Process::checkStatus() const
{
    return checkStatus(false);
}

optional<Process::ExitStatus> Process::checkStatus(bool _waitForExit) const
{
    auto const _ = lock_guard{lock_};

    if (exitStatus_.has_value())
        return exitStatus_;

#if defined(__unix__) || defined(__APPLE__)
    assert(pid_ != -1);
    int status = 0;
    int const rv = waitpid(pid_, &status, _waitForExit ? 0 : WNOHANG);

    if (rv < 0)
        throw runtime_error{ "waitpid: "s + getLastErrorAsString() };
    else if (rv == 0 && !_waitForExit)
        return nullopt;
    else
    {
        pid_ = -1;

        if (WIFEXITED(status))
            return exitStatus_ = ExitStatus{ NormalExit{ WEXITSTATUS(status) } };
        else if (WIFSIGNALED(status))
            return exitStatus_ = ExitStatus{ SignalExit{ WTERMSIG(status) } };
        else if (WIFSTOPPED(status))
            return exitStatus_ = ExitStatus{ SignalExit{ SIGSTOP } };
        else
            // TODO: handle the other WIF....(status) cases.
            throw runtime_error{ "Unknown waitpid() return value." };
    }
#else
    if (_waitForExit)
    {
        if (WaitForSingleObject(processInfo_.hThread, INFINITE /*10 * 1000*/) != S_OK)
            printf("WaitForSingleObject(thr): %s\n", getLastErrorAsString().c_str());
    }
    DWORD exitCode;
    if (!GetExitCodeProcess(processInfo_.hProcess, &exitCode))
        throw runtime_error{ getLastErrorAsString() };
    else if (exitCode == STILL_ACTIVE)
        return exitStatus_;
    else
        return exitStatus_ = ExitStatus{ NormalExit{ static_cast<int>(exitCode) } };
#endif
}

void Process::terminate(TerminationHint _terminationHint)
{
    if (alive())
    {
        #if defined(_WIN32)
        TerminateProcess(nativeHandle(), 1);
        #else
        ::kill(nativeHandle(), _terminationHint == TerminationHint::Hangup ? SIGHUP : SIGTERM);
        #endif
    }
}

Process::ExitStatus Process::wait()
{
    return *checkStatus(true);
}

vector<string> Process::loginShell()
{
#if defined(_WIN32)
    return {"powershell.exe"s};
#else
    if (passwd const* pw = getpwuid(getuid()); pw != nullptr)
    {
#if defined(__APPLE__)
        auto shell = string(pw->pw_shell);
        auto index = shell.rfind('/');
        return {
            "/bin/bash",
            "-c",
            fmt::format("exec -a -{} {}", shell.substr(index + 1, 5), pw->pw_shell)
        };
#else
        return {pw->pw_shell};
#endif
    }
    else
        return {"/bin/sh"s};
#endif
}

FileSystem::path Process::homeDirectory()
{
#if defined(_WIN32)

    if (char const* p = getenv("USERPROFILE"); p && *p)
        return FileSystem::path(p);

    return FileSystem::path("/");
#else
    if (passwd const* pw = getpwuid(getuid()); pw != nullptr)
        return FileSystem::path(pw->pw_dir);
    else
        return FileSystem::path("/");
#endif
}
string Process::workingDirectory(Pty const* _pty) const
{
#if defined(__linux__)
	try
	{
		auto const path = FileSystem::path{fmt::format("/proc/{}/cwd", pid_)};
		auto const cwd = FileSystem::read_symlink(path);
		return cwd.string();
	}
	catch (...)
	{
		// ignore failure, and use default instead.
		return "."s;
	}
#elif defined(__APPLE__)
    try {
        auto vpi = proc_vnodepathinfo{};
        auto const pid = tcgetpgrp(static_cast<UnixPty const*>(_pty)->masterFd());

        if (proc_pidinfo(pid, PROC_PIDVNODEPATHINFO, 0, &vpi, sizeof(vpi)) <= 0) {
            return "."s;
        }

        return string(vpi.pvi_cdir.vip_path);
    } catch (...) {
        return "."s;
    }
#else
	// TODO: Windows
	return "."s;
#endif
}

}  // namespace terminal
