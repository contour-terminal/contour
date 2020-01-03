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
 *
 * Source code was adapted from EchoCon.cpp example source code
 * from Microsoft's Windows Terminal GitHub repository (c) 2018, Microsoft.
 */
#include <terminal/Process.h>
#include <terminal/PseudoTerminal.h>

#include <cstdio>
#include <iostream>
#include <string>
#include <thread>

#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#else
#include <Windows.h>
using ssize_t = SSIZE_T;
#endif

using namespace std;

namespace {

#if !defined(__unix__) && !defined(__APPLE__)
	string GetLastErrorAsString()
	{
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
	}
#endif

	void enableConsoleVT()
	{
#if defined(_MSC_VER)
		HANDLE hConsole = { GetStdHandle(STD_OUTPUT_HANDLE) };
		DWORD consoleMode{};
		GetConsoleMode(hConsole, &consoleMode);
		consoleMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
		if (!SetConsoleMode(hConsole, consoleMode))
			throw runtime_error{"Could not enable Console VT processing. " + GetLastErrorAsString()};
#endif
	}

	ssize_t writeToConsole(char const* _buf, size_t _size)
	{
	#if defined(__unix__) || defined(__APPLE__)
		return ::write(STDOUT_FILENO, _buf, _size);
	#else
		DWORD nwritten{};
        auto const rv = WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), _buf, static_cast<DWORD>(_size), &nwritten, nullptr);
        if (rv == S_OK)
            return static_cast<ssize_t>(nwritten);
        else
            return -1;
	#endif
	}
}

void pipeListener(terminal::PseudoTerminal& pty)
{
	for (;;)
	{
		// Read from the pipe
		char buf[4096]{};
		auto const nread = pty.read(buf, sizeof(buf));
		if (nread == -1)
			break;
		else if (nread > 0)
			writeToConsole(buf, nread);
	}
}

int main()
{
	try
	{
		#if defined(__unix__) || defined(__APPLE__)
		vector<string> args = {"ping", "-c4", "localhost"};
		#else
		vector<string> args = {"ping", "localhost"};
		#endif

		enableConsoleVT();

		terminal::PseudoTerminal pty{terminal::currentWindowSize()};
		thread pipeListenerThread {[&]() { pipeListener(pty); }};
		terminal::Process proc {args[0], args, {}, pty};

        for (bool waiting = true; waiting; )
        {
            terminal::Process::ExitStatus exitStatus = proc.wait();
            if (holds_alternative<terminal::Process::NormalExit>(exitStatus))
            {
                cout << "Process terminated normally with exit code " << get<terminal::Process::NormalExit>(exitStatus).exitCode << '\n';
                waiting = false;
            }
            else if (holds_alternative<terminal::Process::SignalExit>(exitStatus))
            {
                cout << "Process terminated with signal " << get<terminal::Process::SignalExit>(exitStatus).signum << '\n';
                waiting = false;
            }
            else if (holds_alternative<terminal::Process::Suspend>(exitStatus))
                cout << "Process suspended.";
        }

		pty.close();
		pipeListenerThread.join();
	}
	catch (exception const& ex)
	{
		cerr << "Unhandled exception caught. " << ex.what() << endl;
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
