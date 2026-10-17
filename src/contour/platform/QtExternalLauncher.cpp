// SPDX-License-Identifier: Apache-2.0
#include <contour/Logging.hpp>
#include <contour/platform/QtExternalLauncher.hpp>
#include <contour/platform/QtPath.hpp>

#include <vtpty/CommandLine.hpp>
#ifndef _WIN32
    #include <vtpty/DetachedProcess.hpp>
#endif

#include <core/Assert.hpp>

#include <QtCore/QProcess>
#include <QtGui/QDesktopServices>

#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace contour::platform
{

namespace
{
    /// @p text, UTF-8 as the configuration spells it, in Qt's own string type.
    [[nodiscard]] QString fromUtf8(std::string_view text)
    {
        return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
    }

    /// @return The file @p program names: itself where it holds a path, otherwise the one
    ///         isReachableProgram() found on PATH -- on Windows trying %PATHEXT%'s suffixes (`more` is
    ///         more.com), where CreateProcess would try only ".exe".
    [[nodiscard]] QString executableFor(QString const& program)
    {
        auto const namesPath = program.contains(QLatin1Char('/')) || program.contains(QLatin1Char('\\'));
        return namesPath ? program : QStandardPaths::findExecutable(program);
    }

#ifndef _WIN32
    /// Starts @p command detached, placed by @p placement before it runs.
    /// @return Nothing once it was handed to the placement, or why it was not started.
    [[nodiscard]] std::expected<void, SpawnError> startPlaced(vtpty::DetachedCommand const& command,
                                                              vtpty::ProcessPlacement& placement)
    {
        return vtpty::startDetached(command, placement)
            .transform([](int /*pid*/) {})
            .transform_error([&](std::error_code const& error) {
                launcherLog()("Could not start {}: {}.", command.image, error.message());
                return SpawnError::StartFailed;
            });
    }
#endif
} // namespace

QtExternalLauncher::QtExternalLauncher(std::shared_ptr<vtpty::ProcessPlacement> placement):
    _placement { std::move(placement) }
{
    // A construction-time requirement rather than a runtime error: a launcher built without one is a
    // wiring mistake, and there is no answer to give a user about it.
    Require(_placement != nullptr);
}

std::expected<void, LaunchError> QtExternalLauncher::openUrl(QUrl const& url)
{
    if (!isOpenable(url))
        return std::unexpected(LaunchError::InvalidUrl);

    // Qt reports one bool for two different failures -- no handler found, and a handler that could
    // not be started -- so the more specific of the two is not available to report here.
    if (!QDesktopServices::openUrl(url))
        return std::unexpected(LaunchError::NoHandler);

    return {};
}

std::expected<void, SpawnError> QtExternalLauncher::runDetached(QString const& program,
                                                                QStringList const& arguments)
{
    if (!isReachableProgram(program))
        return std::unexpected(SpawnError::NotFound);

#ifdef _WIN32
    if (!QProcess::startDetached(program, arguments))
        return std::unexpected(SpawnError::StartFailed);
    return {};
#else
    // Its standard streams are this process's, as QProcess::startDetached() leaves them.
    auto argv = std::vector<std::string> { program.toStdString() };
    for (auto const& argument: arguments)
        argv.push_back(argument.toStdString());
    return startPlaced(vtpty::DetachedCommand { .image = executableFor(program).toStdString(),
                                                .argv = std::move(argv),
                                                .standardInput = {},
                                                .output = vtpty::DetachedOutput::Inherited },
                       *_placement);
#endif
}

std::expected<void, SpawnError> QtExternalLauncher::runDetachedWithInput(
    std::string const& commandLine, std::filesystem::path const& inputFile)
{
    // Read as a session's configured program is, with this machine answering what names a program.
    auto const invocation = vtpty::invocationToRun(
        commandLine, {}, [](std::string_view name) { return isReachableProgram(fromUtf8(name)); });

    auto const program = fromUtf8(invocation.program);
    if (!isReachableProgram(program))
        return std::unexpected(SpawnError::NotFound);

#ifdef _WIN32
    auto arguments = QStringList {};
    for (auto const& argument: invocation.arguments)
        arguments.append(fromUtf8(argument));

    auto process = QProcess {};
    process.setProgram(executableFor(program));
    process.setArguments(arguments);
    // What the configuration wrote after the program, unchanged: the child splits it by its own rules.
    if (!invocation.writtenArguments.empty())
        process.setNativeArguments(fromUtf8(invocation.writtenArguments));
    process.setStandardInputFile(toQString(inputFile));
    process.setStandardOutputFile(QProcess::nullDevice());
    process.setStandardErrorFile(QProcess::nullDevice());

    // Detached: the child is not this process's, so nothing ends it at exit. A program that cannot be
    // started is reported at once; nothing waits for one that can.
    if (!process.startDetached())
        return std::unexpected(SpawnError::StartFailed);
    return {};
#else
    // Detached, and placed: the child is not this process's, so nothing ends it at exit. A program that
    // cannot be started is reported at once; nothing waits for one that can.
    auto argv = std::vector<std::string> { invocation.program };
    argv.insert(argv.end(), invocation.arguments.begin(), invocation.arguments.end());
    return startPlaced(vtpty::DetachedCommand { .image = executableFor(program).toStdString(),
                                                .argv = std::move(argv),
                                                .standardInput = inputFile,
                                                .output = vtpty::DetachedOutput::Discarded },
                       *_placement);
#endif
}

} // namespace contour::platform
