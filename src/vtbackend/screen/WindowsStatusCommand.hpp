// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QProcess>
#include <cstdlib>
#include <string>
#include <string_view>

#include <Windows.h>
#include <fcntl.h>
#include <locale.h>

namespace vtbackend::detail
{

/// Captures a Windows shell command without creating a console window.
inline std::string runWindowsStatusCommand(std::string_view command)
{
    QProcess child;
    child.setProgram(qEnvironmentVariable("COMSPEC", "cmd.exe"));
    auto const text = ___lc_codepage_func() == CP_UTF8
                          ? QString::fromUtf8(command.data(), static_cast<qsizetype>(command.size()))
                          : QString::fromLocal8Bit(command.data(), static_cast<qsizetype>(command.size()));
    child.setNativeArguments(QStringLiteral("/c ") + text);
    child.setInputChannelMode(QProcess::ForwardedInputChannel);
    child.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    child.setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments* args) { args->flags |= CREATE_NO_WINDOW; });
    child.start(QIODevice::ReadOnly);
    if (!child.waitForStarted(-1))
        return child.errorString().toStdString();
    if (!child.waitForFinished(-1) && child.state() != QProcess::NotRunning)
        return child.errorString().toStdString();
    auto result = child.readAllStandardOutput().toStdString();
    // Preserve the original CRT text stream's Ctrl-Z EOF and first-line CRLF handling.
    int mode = _O_TEXT;
    _get_fmode(&mode);
    if (mode == _O_TEXT)
        if (auto const eof = result.find('\x1a'); eof != std::string::npos)
            result.erase(eof);
    if (auto const newline = result.find('\n'); newline != std::string::npos)
    {
        result.erase(newline);
        if (mode == _O_TEXT && !result.empty() && result.back() == '\r')
            result.pop_back();
    }
    return result;
}
} // namespace vtbackend::detail
