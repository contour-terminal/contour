// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QtCore/QStringList>
#include <QtCore/QUrl>

namespace contour
{

/// Launches external resources on behalf of a terminal session: opening URLs/documents in the
/// desktop's default handler and running detached or blocking child processes.
///
/// This is the part of a session that reaches OUT of the process — into the desktop environment
/// (a browser, a file manager, an editor) — and it is an interface per the project's
/// dependency-injection principle: spawning a browser or a child `contour` is process I/O, so it
/// must be reached through an abstraction rather than the concrete static Qt calls
/// (`QDesktopServices::openUrl`, `QProcess::execute`/`startDetached`). Production wires
/// QtExternalLauncher; tests inject a recording launcher and assert the routing/validation of the
/// open-document, follow-hyperlink, and spawn-terminal actions without actually launching anything.
class ExternalLauncher
{
  public:
    virtual ~ExternalLauncher() = default;

    /// Opens @p url in the desktop's default handler (browser, file manager, editor, ...).
    /// @param url The resource to open.
    /// @return true if the platform accepted the request.
    [[nodiscard]] virtual bool openUrl(QUrl const& url) = 0;

    /// Starts @p program with @p arguments as a detached background process (fire-and-forget).
    /// @param program The executable path.
    /// @param arguments The argument list.
    /// @return true if the process was started.
    virtual bool runDetached(QString const& program, QStringList const& arguments) = 0;

    /// Runs @p program with @p arguments and blocks until it exits (used where the caller wants the
    /// child — e.g. an `$EDITOR` — to run to completion).
    /// @param program The executable path.
    /// @param arguments The argument list.
    /// @return The process exit code, or a negative value if it could not be started.
    virtual int execute(QString const& program, QStringList const& arguments) = 0;
};

} // namespace contour
