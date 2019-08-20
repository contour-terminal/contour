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

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>

#if !defined(_MSC_VER)
#include <pty.h>
#include <utmp.h>
#include <pwd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace std;

namespace {
	string GetLastErrorAsString()
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

WindowSize currentWindowSize()
{
#if defined(__unix__)
    auto w = winsize{};

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == -1)
		throw runtime_error{strerror(errno)};

    return WindowSize{w.ws_col, w.ws_row};
#else
	CONSOLE_SCREEN_BUFFER_INFO csbi{};
	HANDLE hConsole{ GetStdHandle(STD_OUTPUT_HANDLE) };
	if (GetConsoleScreenBufferInfo(hConsole, &csbi))
		return WindowSize{
			static_cast<unsigned short>(csbi.srWindow.Right - csbi.srWindow.Left + 1),
			static_cast<unsigned short>(csbi.srWindow.Bottom - csbi.srWindow.Top + 1)
	};
	else
		throw runtime_error{GetLastErrorAsString()};
#endif
}

std::string Process::loginShell()
{
#if defined(_MSC_VER)
	return "cmd.exe"s;
#else
    if (passwd const* pw = getpwuid(getuid()); pw != nullptr)
        return pw->pw_shell;
    else
        return "/bin/sh"s;
#endif
}

PseudoTerminal::PseudoTerminal(WindowSize const& _windowSize)
{
#if defined(__unix__)
	// See https://code.woboq.org/userspace/glibc/login/forkpty.c.html
	winsize const ws{_windowSize.rows, _windowSize.columns, 0, 0};
	// TODO: termios term{};
	if (openpty(&master_, &slave_, nullptr, /*&term*/ nullptr, &ws) < 0)
		throw runtime_error{ "Failed to open PTY. " + GetLastErrorAsString() };
#else
	master_ = INVALID_HANDLE_VALUE;
	input_ = INVALID_HANDLE_VALUE;
	output_ = INVALID_HANDLE_VALUE;

	HANDLE hPipePTYIn{ INVALID_HANDLE_VALUE };
	HANDLE hPipePTYOut{ INVALID_HANDLE_VALUE };

	// Create the pipes to which the ConPTY will connect to
	if (!CreatePipe(&hPipePTYIn, &output_, NULL, 0))
		throw runtime_error{ GetLastErrorAsString() };

	if (!CreatePipe(&input_, &hPipePTYOut, NULL, 0))
	{
		CloseHandle(hPipePTYIn);
		throw runtime_error{ GetLastErrorAsString() };
	}

	// Create the Pseudo Console of the required size, attached to the PTY-end of the pipes
	HRESULT hr = CreatePseudoConsole(
		{ static_cast<SHORT>(_windowSize.columns), static_cast<SHORT>(_windowSize.rows) },
		hPipePTYIn,
		hPipePTYOut,
		0,
		&master_
	);

	if (hPipePTYIn != INVALID_HANDLE_VALUE)
		CloseHandle(hPipePTYIn);

	if (hPipePTYOut != INVALID_HANDLE_VALUE)
		CloseHandle(hPipePTYOut);

	if (hr != S_OK)
		throw runtime_error{ GetLastErrorAsString() };
#endif
}

PseudoTerminal::~PseudoTerminal()
{
	release();
}

void PseudoTerminal::release()
{
#if defined(__unix__)
	if (master_ >= 0)
	{
		close(master_);
		master_ = -1;
	}
#else
	if (master_ != INVALID_HANDLE_VALUE)
	{
		ClosePseudoConsole(master_);
		master_ = INVALID_HANDLE_VALUE;
	}
	if (input_ != INVALID_HANDLE_VALUE)
	{
		CloseHandle(input_);
		input_ = INVALID_HANDLE_VALUE;
	}
	if (output_ != INVALID_HANDLE_VALUE)
	{
		CloseHandle(output_);
		output_ = INVALID_HANDLE_VALUE;
	}
#endif
}

auto PseudoTerminal::read(char* buf, size_t size) -> ssize_t
{
#if defined(__unix__)
	return ::read(master_, buf, size);
#else
	DWORD nread{};
	if (ReadFile(input_, buf, static_cast<DWORD>(size), &nread, nullptr))
		return nread;
	else
		return -1;
#endif
}

auto PseudoTerminal::write(char const* buf, size_t size) -> ssize_t
{
#if defined(__unix__)
	return ::write(master_, buf, size);
#else
	DWORD nwritten{};
	if (WriteFile(output_, buf, static_cast<DWORD>(size), &nwritten, nullptr))
		return nwritten;
	else
		return -1;
#endif
}

/////////////////////////////////////////////////////////////////////////////////

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
		throw runtime_error{ strerror(errno) };
	case 0:  // in child
	{
		close(_pty.master());
		if (login_tty(_pty.slave()) < 0)
			_exit(EXIT_FAILURE);

		char** argv = new char* [_args.size() + 1];
		for (size_t i = 0; i < _args.size(); ++i)
			argv[i] = const_cast<char*>(_args[i].c_str());

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
		throw runtime_error{ GetLastErrorAsString() };
#endif
}

Process::~Process()
{
#if defined(__unix__)
	// TODO
#else
	CloseHandle(processInfo_.hThread);
	CloseHandle(processInfo_.hProcess);

	DeleteProcThreadAttributeList(startupInfo_.lpAttributeList);
	free(startupInfo_.lpAttributeList);
#endif
}

Process::ExitStatus Process::wait()
{
#if defined(__unix__)
	int status = 0;
	waitpid(pid_, &status, 0);

	if (WIFEXITED(status))
		return NormalExit{ WEXITSTATUS(status) };
	else if (WIFSIGNALED(status))
		return SignalExit{ WTERMSIG(status) };
	else if (WIFSTOPPED(status))
		return Suspend{};
	else if (WIFCONTINUED(status))
		return Resume{};
	else
		throw runtime_error{ "Unknown waitpid() return value." };
#else
	if (WaitForSingleObject(processInfo_.hThread, INFINITE /*10 * 1000*/) != S_OK)
		printf("WaitForSingleObject(thr): %s\n", GetLastErrorAsString().c_str());
	//if (WaitForSingleObject(processInfo_.hProcess, INFINITE) != S_OK)
	//	printf("WaitForSingleObject(proc): %s\n", GetLastErrorAsString().c_str());
	DWORD exitCode;
	if (GetExitCodeProcess(processInfo_.hProcess, &exitCode))
		return NormalExit{ static_cast<int>(exitCode) };
	else
		throw runtime_error{ GetLastErrorAsString() };
#endif
}

}  // namespace terminal
