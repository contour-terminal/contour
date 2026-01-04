// SPDX-License-Identifier: Apache-2.0

#if defined(CONTOUR_FRONTEND_GUI)
    #include <contour/ContourGuiApp.h>
#else
    #include <contour/ContourApp.h>
#endif

#include <QtCore/QByteArray>
#include <QtCore/QString>

#if __has_include(<QtCore/QtLogging>)
    #include <QtCore/QtLogging>
#endif

#if defined(_WIN32)
    #include <cstdio>
    #include <iostream>

    #include <Windows.h>
    #include <fcntl.h>
    #include <io.h>
#endif

using namespace std;

namespace
{
#if defined(_WIN32)
bool is_a_console(HANDLE h)
{
    auto modeDummy = DWORD { 0 };
    return GetConsoleMode(h, &modeDummy);
}

void reopen_console_handle(DWORD std, int fd, FILE* stream)
{
    HANDLE handle = GetStdHandle(std);
    if (!is_a_console(handle))
        return;
    if (fd == 0)
        freopen("CONIN$", "rt", stream);
    else
        freopen("CONOUT$", "wt", stream);

    setvbuf(stream, NULL, _IONBF, 0);

    // Set the low-level FD to the new handle value, since mp_subprocess2
    // callers might rely on low-level FDs being set. Note, with this
    // method, fileno(stdin) != STDIN_FILENO, but that shouldn't matter.
    int unbound_fd = -1;
    if (fd == 0)
        unbound_fd = _open_osfhandle((intptr_t) handle, _O_RDONLY);
    else
        unbound_fd = _open_osfhandle((intptr_t) handle, _O_WRONLY);

    // dup2 will duplicate the underlying handle. Don't close unbound_fd,
    // since that will close the original handle.
    if (unbound_fd != -1)
        dup2(unbound_fd, fd);
}

template <typename... Ts>
void clearAll(Ts&&... streams)
{
    (streams.clear(), ...);
}

void tryAttachConsole()
{
    if (AttachConsole(ATTACH_PARENT_PROCESS) == FALSE)
        return;

    // We have a console window. Redirect input/output streams to that console's
    // low-level handles, so things that use stdio work later on.
    reopen_console_handle(STD_INPUT_HANDLE, 0, stdin);
    reopen_console_handle(STD_OUTPUT_HANDLE, 1, stdout);
    reopen_console_handle(STD_ERROR_HANDLE, 2, stderr);

    clearAll(cin, cout, cerr, clog, wcin, wcout, wcerr, wclog);
}
#endif

void qtCustomMessageOutput(QtMsgType type, const QMessageLogContext& context, const QString& msg)
{
    QByteArray const localMsg = msg.toLocal8Bit();
    switch (type)
    {
        case QtDebugMsg:
            fprintf(stderr,
                    "Debug[%s]: %s (%s:%u, %s)\n",
                    context.category,
                    localMsg.constData(),
                    context.file,
                    context.line,
                    context.function);
            break;
        case QtInfoMsg:
            fprintf(stderr,
                    "Info: %s (%s:%u, %s)\n",
                    localMsg.constData(),
                    context.file,
                    context.line,
                    context.function);
            break;
        case QtWarningMsg:
            fprintf(stderr,
                    "Warning: %s (%s:%u, %s)\n",
                    localMsg.constData(),
                    context.file,
                    context.line,
                    context.function);
            break;
        case QtCriticalMsg:
            fprintf(stderr,
                    "Critical: %s (%s:%u, %s)\n",
                    localMsg.constData(),
                    context.file,
                    context.line,
                    context.function);
            break;
        case QtFatalMsg:
            fprintf(stderr,
                    "Fatal: %s (%s:%u, %s)\n",
                    localMsg.constData(),
                    context.file,
                    context.line,
                    context.function);
            abort();
    }
}

} // namespace

#if defined(_WIN32)
int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow)
{
    UNREFERENCED_PARAMETER(hInstance);
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);
    UNREFERENCED_PARAMETER(nCmdShow);

    int argc;
    LPWSTR* argvW = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argvW)
        return -1;

    std::vector<std::string> args;
    args.reserve(argc);
    for (int i = 0; i < argc; ++i)
    {
        int size_needed = WideCharToMultiByte(CP_UTF8, 0, argvW[i], -1, NULL, 0, NULL, NULL);
        std::string str(size_needed ? size_needed - 1 : 0, 0);
        WideCharToMultiByte(CP_UTF8, 0, argvW[i], -1, &str[0], size_needed, NULL, NULL);
        args.push_back(std::move(str));
    }
    LocalFree(argvW);

    std::vector<const char*> argvC;
    argvC.reserve(args.size());
    for (const auto& arg: args)
        argvC.push_back(arg.c_str());

    // Forward to main
    extern int main(int argc, char const* argv[]);
    return main(static_cast<int>(argvC.size()), argvC.data());
}
#endif

void SimpleFileLogger(std::string const& msg)
{
#if defined(_WIN32)
    char tempPath[MAX_PATH];
    if (GetTempPathA(MAX_PATH, tempPath))
    {
        std::string path = std::string(tempPath) + "contour_debug.txt";
        FILE* f = fopen(path.c_str(), "a");
        if (f)
        {
            fprintf(f, "[%u] %s\n", GetCurrentProcessId(), msg.c_str());
            fclose(f);
        }
    }
#endif
}

int main(int argc, char const* argv[])
{
    // #if defined(_WIN32)
    //     MessageBoxA(nullptr, "Contour Main Reached!", "Debug", MB_OK);
    // #endif

#if defined(_WIN32)
    tryAttachConsole();
#endif

    // Normalize arguments: Windows passes -Embedding, but our CLI might expect --embedding
    std::vector<std::string> args;
    args.reserve(argc);
    for (int i = 0; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "-Embedding" || arg == "/Embedding")
            args.emplace_back("--embedding");
        else
            args.emplace_back(arg);
    }

    // Create new argv array
    std::vector<char const*> newArgv;
    newArgv.reserve(args.size());
    for (auto const& s: args)
        newArgv.emplace_back(s.c_str());

    SimpleFileLogger("Contour started.");
    for (int i = 0; i < newArgv.size(); ++i)
        SimpleFileLogger(std::format("Arg {}: {}", i, newArgv[i]));

    qInstallMessageHandler(qtCustomMessageOutput);

#if defined(CONTOUR_FRONTEND_GUI)
    contour::ContourGuiApp app;
#else
    contour::ContourApp app;
#endif

    return app.run(static_cast<int>(newArgv.size()), newArgv.data());
}
