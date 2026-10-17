// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/QtExternalLauncher.hpp>
#include <contour/platform/QtPath.hpp>

#include <vtpty/CommandLine.hpp>

#include <QtCore/QProcess>
#include <QtGui/QDesktopServices>

#include <string_view>

namespace contour::platform
{

namespace
{
    /// @p text, UTF-8 as the configuration spells it, in Qt's own string type.
    [[nodiscard]] QString fromUtf8(std::string_view text)
    {
        return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
    }
} // namespace

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

    if (!QProcess::startDetached(program, arguments))
        return std::unexpected(SpawnError::StartFailed);

    return {};
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

    auto arguments = QStringList {};
    for (auto const& argument: invocation.arguments)
        arguments.append(fromUtf8(argument));

    auto process = QProcess {};
    // A bare name runs as isReachableProgram() found it, on PATH: on Windows that tries %PATHEXT%'s
    // suffixes (`more` is more.com), where CreateProcess would try only ".exe".
    auto const namesPath = program.contains(QLatin1Char('/')) || program.contains(QLatin1Char('\\'));
    process.setProgram(namesPath ? program : QStandardPaths::findExecutable(program));
    process.setArguments(arguments);
#ifdef _WIN32
    // What the configuration wrote after the program, unchanged: the child splits it by its own rules.
    if (!invocation.writtenArguments.empty())
        process.setNativeArguments(fromUtf8(invocation.writtenArguments));
#endif
    process.setStandardInputFile(toQString(inputFile));
    process.setStandardOutputFile(QProcess::nullDevice());
    process.setStandardErrorFile(QProcess::nullDevice());

    // Detached: the child is not this process's, so nothing ends it at exit. A program that cannot be
    // started is reported at once; nothing waits for one that can.
    if (!process.startDetached())
        return std::unexpected(SpawnError::StartFailed);

    return {};
}

} // namespace contour::platform
