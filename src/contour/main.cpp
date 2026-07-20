// SPDX-License-Identifier: Apache-2.0

#ifdef CONTOUR_FRONTEND_GUI
    #include <contour/ContourGuiApp.h>
#else
    #include <contour/ContourApp.h>
#endif

#include <contour/BenignQtMessages.h>

#include <crispy/SuppressWindowsDialogs.hpp>

#include <QtCore/QByteArray>
#include <QtCore/QString>

#if __has_include(<QtCore/QtLogging>)
    #include <QtCore/QtLogging>
#endif

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <print>
#include <string>
#include <string_view>

#ifdef _WIN32
    #include <cstdio>
    #include <iostream>

    #include <Windows.h>
    #include <fcntl.h>
    #include <io.h>
#endif

using namespace std;

namespace
{
#ifdef _WIN32
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

/// Human-readable severity prefix for a Qt message type.
/// @param type The Qt message severity.
/// @return The prefix printed ahead of the message text.
[[nodiscard]] constexpr std::string_view severityName(QtMsgType type) noexcept
{
    switch (type)
    {
        case QtDebugMsg: return "Debug";
        case QtInfoMsg: return "Info";
        case QtWarningMsg: return "Warning";
        case QtCriticalMsg: return "Critical";
        case QtFatalMsg: return "Fatal";
    }
    return "Unknown";
}

void qtCustomMessageOutput(QtMsgType type, QMessageLogContext const& context, QString const& msg)
{
    QByteArray const localMsg = msg.toLocal8Bit();

    // Drop known-benign, source-unfixable Qt-internal noise (see BenignQtMessages.h) before it reaches
    // the user, so it never dilutes Contour's real diagnostics. Never suppress a fatal message: Qt
    // considers it terminal, and swallowing it would skip the abort() below and leave the process running
    // in an undefined state.
    if (type != QtFatalMsg
        && contour::isBenignQtMessage(
            std::string_view { localMsg.constData(), static_cast<size_t>(localMsg.size()) }))
        return;

    // Qt only fills context.file/function when the caller was compiled with QT_MESSAGELOGCONTEXT;
    // std::format has no printf-style "(null)" fallback for a null char const*, so normalise here.
    auto const orNull = [](char const* text) {
        return text != nullptr ? text : "(null)";
    };
    auto const severity = type == QtDebugMsg ? std::format("Debug[{}]", orNull(context.category))
                                             : std::string { severityName(type) };

    std::println(stderr,
                 "{}: {} ({}:{}, {})",
                 severity,
                 localMsg.constData(),
                 orNull(context.file),
                 context.line,
                 orNull(context.function));

    // Qt considers a fatal message terminal; keep aborting rather than returning into an undefined state.
    if (type == QtFatalMsg)
        abort();
}
} // namespace

int main(int argc, char const* argv[])
{
#ifdef _WIN32
    tryAttachConsole();

    // Route CRT assert/abort/crash reporting to stderr instead of a modal dialog when no debugger is
    // attached (CI, scripted GUI/verification runs), so an assertion can never block a headless run.
    // Under an attached debugger the dialogs and debug breaks are kept so interactive debugging works.
    if (!IsDebuggerPresent())
        crispy::suppressWindowsDialogs();
#endif

    qInstallMessageHandler(qtCustomMessageOutput);

#ifdef CONTOUR_FRONTEND_GUI
    contour::ContourGuiApp app;
#else
    contour::ContourApp app;
#endif

    return app.run(argc, argv);
}
