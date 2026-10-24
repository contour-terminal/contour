// SPDX-License-Identifier: Apache-2.0

// Everything Qt in this file serves the message handler installed below, which exists to tidy up
// what Qt itself prints. A build without the GUI frontend links no Qt, so nothing there emits a Qt
// message and there is no handler to install.
#ifdef CONTOUR_FRONTEND_GUI
    #include <contour/BenignQtMessages.hpp>
    #include <contour/ContourGuiApp.hpp>

    #include <QtCore/QByteArray>
    #include <QtCore/QString>

    #if __has_include(<QtCore/QtLogging>)
        #include <QtCore/QtLogging>
    #endif
#else
    #include <contour/cli/ContourApp.hpp>
#endif

#include <core/Environment.hpp>
#include <core/platform/Clock.hpp>
#include <core/testing/SuppressWindowsDialogs.hpp>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <system_error>

#ifdef _WIN32
    #include <core/WideString.hpp>

    #include <iostream>
    #include <span>
    #include <vector>

    #include <Windows.h>
    #include <fcntl.h>
    #include <io.h>
    #include <shellapi.h>
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

/// The command line as UTF-8, which every string here is spelled in: the narrow argv main() is given is
/// in the ANSI code page, which spells most of Unicode not at all -- "C:\Users\李\x.yml" arrives as
/// "C:\Users\?\x.yml", and one it can spell arrives as bytes no UTF-8 reader understands.
/// @return Each argument, as CommandLineToArgvW() splits the command line; empty should that fail.
[[nodiscard]] std::vector<std::string> utf8Arguments()
{
    auto count = 0;
    auto* const wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wide == nullptr)
        return {};
    auto arguments = std::vector<std::string> {};
    for (wchar_t const* const argument: std::span { wide, static_cast<std::size_t>(count) })
        arguments.push_back(core::toUtf8(argument));
    LocalFree(static_cast<HLOCAL>(wide));
    return arguments;
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

#ifdef CONTOUR_FRONTEND_GUI
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
    try
    {
        auto const severity = type == QtDebugMsg ? std::format("Debug[{}]", orNull(context.category))
                                                 : std::string { severityName(type) };
        std::println(stderr,
                     "{}: {} ({}:{}, {})",
                     severity,
                     localMsg.constData(),
                     orNull(context.file),
                     context.line,
                     orNull(context.function));
    }
    catch (std::system_error const&)
    {
        // A GUI process can have an unavailable stderr. A failed diagnostic write must not
        // propagate through Qt's message handler; there is no usable stream to report it to.
        if (type != QtFatalMsg)
            return;
    }

    // Qt considers a fatal message terminal; keep aborting rather than returning into an undefined state.
    if (type == QtFatalMsg)
        abort();
}
#endif
} // namespace

int main(int argc, char const* argv[])
{
#ifdef CONTOUR_FRONTEND_GUI
    // The origin of the startup timeline: sampled before anything else runs.
    auto const processStart = core::platform::defaultSteadyClock().now();
#endif

#ifdef _WIN32
    // Ahead of the app, which keeps argv for as long as it lives.
    auto const arguments = utf8Arguments();
    auto argumentPointers = std::vector<char const*> {};
    for (auto const& argument: arguments)
        argumentPointers.push_back(argument.c_str());
    if (!argumentPointers.empty())
    {
        argc = static_cast<int>(argumentPointers.size());
        argv = argumentPointers.data();
    }

    tryAttachConsole();

    // Route CRT assert/abort/crash reporting to stderr instead of a modal dialog when no debugger is
    // attached (CI, scripted GUI/verification runs), so an assertion can never block a headless run.
    // Under an attached debugger the dialogs and debug breaks are kept so interactive debugging works.
    if (!IsDebuggerPresent())
        core::testing::suppressWindowsDialogs();
#endif

    // This is the process's composition root, and the environment is the one ambient resource
    // everything below reads: the app takes it here, once, and hands it on to what it builds.
    auto& environment = core::defaultEnvironment();

#ifdef CONTOUR_FRONTEND_GUI
    qInstallMessageHandler(qtCustomMessageOutput);

    contour::ContourGuiApp app { environment,
                                 nullptr, // sessionFactory
                                 nullptr, // externalLauncher
                                 nullptr, // layoutStore
                                 nullptr, // commandHistoryStore
                                 nullptr, // speechSynthesizer
                                 nullptr, // commandOutputFiles
                                 vtbackend::TerminalClocks::system(),
                                 std::nullopt, // hostIdentity
                                 nullptr,      // fileSystem
                                 nullptr,      // processPlacement
                                 std::nullopt, // linkServices
                                 nullptr,      // taskbarProgress
                                 nullptr,      // windowPositionStore
                                 contour::platform::AppearanceServices {},
                                 processStart };
#else
    // Without the GUI frontend there is no window to open, so `contour client` is gone with it;
    // what remains is `contour daemon` and the CLI verbs, which need nothing beyond Qt Core.
    contour::cli::ContourApp app { environment };
#endif

    return app.run(argc, argv);
}
