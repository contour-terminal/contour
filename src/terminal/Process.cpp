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
#include <fmt/format.h>
#include <terminal/util/stdfs.h>
#include <terminal/util/overloaded.h>

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
#include <utmp.h>
#include <pwd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#else
#include <direct.h>
#include <errno.h>
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
	#endif
} // anonymous namespace

#if !defined(_WIN32)
static termios getTerminalSettings(int fd)
{
    termios tio;
    tcgetattr(fd, &tio);
    return tio;
}

static termios constructTerminalSettings(int fd)
{
    auto tio = getTerminalSettings(fd);

    // input flags
    tio.c_iflag |= IGNBRK;    // Ignore Break condition on input.
    tio.c_iflag &= ~IXON;     // Disable CTRL-S / CTRL-Q on output.
    tio.c_iflag &= ~IXOFF;    // Disable CTRL-S / CTRL-Q on input.
    tio.c_iflag &= ~ICRNL;    // Ensure CR isn't translated to NL.
    tio.c_iflag &= ~INLCR;    // Ensure NL isn't translated to CR.
    tio.c_iflag &= ~IGNCR;    // Ensure CR isn't ignored.
    tio.c_iflag &= ~IMAXBEL;  // Ensure beeping on full input buffer isn't enabled.
    tio.c_iflag &= ~ISTRIP;   // Ensure stripping of 8th bit on input isn't enabled.

    // output flags
    tio.c_oflag &= ~OPOST;   // Don't enable implementation defined output processing.
    tio.c_oflag &= ~ONLCR;   // Don't map NL to CR-NL.
    tio.c_oflag &= ~OCRNL;   // Don't map CR to NL.
    tio.c_oflag &= ~ONLRET;  // Don't output CR.

    // control flags

    // local flags
    tio.c_lflag &= ~IEXTEN;  // Don't enable implementation defined input processing.
    tio.c_lflag &= ~ICANON;  // Don't enable line buffering (Canonical mode).
    tio.c_lflag &= ~ECHO;    // Don't echo input characters.
    tio.c_lflag &= ~ISIG;    // Don't generate signal upon receiving characters for
                                // INTR, QUIT, SUSP, DSUSP.

    // special characters
    tio.c_cc[VMIN] = 1;   // Report as soon as 1 character is available.
    tio.c_cc[VTIME] = 0;  // Disable timeout (no need).

    return tio;
}
#endif

Process::Process(string const& _path,
                 vector<string> const& _args,
                 Environment const& _env,
                 PseudoTerminal& _pty)
{
#if defined(__unix__) || defined(__APPLE__)
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

            auto tio = constructTerminalSettings(_pty.master());
            if (tcsetattr(_pty.master(), TCSANOW, &tio) == 0)
                tcflush(_pty.master(), TCIOFLUSH);

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
    {
        cmd += ' ';
        if (_args[i].find(' ') != std::string::npos)
            cmd += '\"' + _args[i] + '\"';
        else
            cmd += _args[i];
    }

	auto const envScope = InheritingEnvBlock{_env};

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

Process::Process(
    string const& _path,
    vector<string> const& _args,
    Environment const& _env,
	std::string const& _cwd,
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

			if (chdir(_cwd.c_str()) < 0)
			{
				printf("Failed to chdir to \"%s\". %s\n", _cwd.c_str(), strerror(errno));
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

    BOOL success = CreateProcess(
        nullptr,                            // No module name - use Command Line
        const_cast<LPSTR>(cmd.c_str()),     // Command Line
        nullptr,                            // Process handle not inheritable
        nullptr,                            // Thread handle not inheritable
        FALSE,                              // Inherit handles
        EXTENDED_STARTUPINFO_PRESENT,       // Creation flags
        nullptr,                            // Use parent's environment block
        _cwd.c_str(),                       // Use parent's starting directory
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
    exitStatus_.reset();
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
    if (exitStatus_.has_value())
        return exitStatus_;

#if defined(__unix__) || defined(__APPLE__)
    assert(pid_ != -1);
    int status = 0;
    int const rv = waitpid(pid_, &status, _waitForExit ? 0 : WNOHANG);

    if (rv < 0)
        throw runtime_error{ getLastErrorAsString() };
    else if (rv == 0)
        return nullopt;
    else
    {
        pid_ = -1;

        if (WIFEXITED(status))
            return exitStatus_ = ExitStatus{ NormalExit{ WEXITSTATUS(status) } };
        else if (WIFSIGNALED(status))
            return exitStatus_ = ExitStatus{ SignalExit{ WTERMSIG(status) } };
        else if (WIFSTOPPED(status))
            return exitStatus_ = ExitStatus{ Suspend{} };
        else
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

Process::ExitStatus Process::wait()
{
    return *checkStatus(true);
}

std::string Process::loginShell()
{
#if defined(_WIN32)
    return "powershell.exe"s;
#else
    if (passwd const* pw = getpwuid(getuid()); pw != nullptr)
        return pw->pw_shell;
    else
        return "/bin/sh"s;
#endif
}

string Process::workingDirectory() const
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
#else
	// TODO: Apple, Windows
	return "."s;
#endif
}

Process::ExitStatus Process::waitForExit()
{
    while (true)
        if (visit(overloaded{[&](NormalExit) { return true; },
                             [&](SignalExit) { return true; },
                             [&](Suspend) { return false; },
                  },
                  wait()))
            break;

    return exitStatus_.value();
}

}  // namespace terminal
