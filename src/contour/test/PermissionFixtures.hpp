// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/session/PermissionQueue.hpp>
#include <contour/session/TerminalSession.hpp>

#include <QtCore/QObject>
#include <QtCore/QString>

namespace contour::test
{

/// The id of the question @p session shows, as its dialog reads it.
[[nodiscard]] inline int shownQuestionId(session::TerminalSession const& session)
{
    return session.pendingPermission().value(QStringLiteral("id")).toInt();
}

/// Answers the question @p session shows, as its dialog would.
inline void answerShown(session::TerminalSession& session,
                        session::PermissionDecision decision,
                        session::PermissionMemory memory)
{
    session.answerPermission(shownQuestionId(session), decision, memory);
}

/// Counts into @p asks each time @p session shows @p ask -- a dialog opening, or the open one changing what
/// it says. The connection lives with the session, which every caller outlives.
inline void countAsks(session::TerminalSession& session, session::PermissionAsk ask, int& asks)
{
    QObject::connect(&session, &session::TerminalSession::pendingPermissionChanged, [&session, ask, &asks] {
        if (session.pendingPermissionAsk() == ask)
            ++asks;
    });
}

} // namespace contour::test
