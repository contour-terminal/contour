// EchoCon.cpp : Entry point for the EchoCon Pseudo-Console sample application.
// Copyright � 2018, Microsoft

#include <terminal/Process.h>

#include <cstdio>
#include <iostream>
#include <string>
#include <thread>

#if defined(__unix__)
#include <unistd.h>
#else
#include <Windows.h>
#endif

using namespace std;

namespace {

#if !defined(__unix__)
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

	void writeToConsole(char const* buf, size_t n)
	{
	#if defined(__unix__)
		::write(STDOUT_FILENO, buf, n);
	#else
		DWORD nwritten{};
		WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), buf, static_cast<DWORD>(n), &nwritten, nullptr);
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
		#if defined(__unix__)
		vector<string> args = {"ping", "-c4", "localhost"};
		#else
		vector<string> args = {"ping", "localhost"};
		#endif

		enableConsoleVT();

		terminal::PseudoTerminal pty{terminal::currentWindowSize()};
		thread pipeListenerThread {[&]() { pipeListener(pty); }};
		terminal::Process proc {pty, args[0], args, {}};

		terminal::Process::ExitStatus exitStatus = proc.wait();
		pty.release();
		pipeListenerThread.join();
	}
	catch (exception const& ex)
	{
		cerr << "Unhandled exception caught. " << ex.what() << endl;
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
