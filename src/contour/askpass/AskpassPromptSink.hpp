// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Who shows ssh's prompts to the user. The broker decides what a prompt is and who may ask; the sink only
/// puts it in front of someone.

#include <contour/askpass/AskpassProtocol.hpp>

#include <QtCore/QString>

#include <cstdint>
#include <functional>

namespace contour::askpass
{

/// Names one prompt for as long as it is open: the broker hands the sink the id with the prompt, and names
/// the prompt again when its helper goes away.
using AskpassPromptId = std::uint64_t;

/// Shows a prompt and tells what the user made of it. Called on the thread the broker was made on (the GUI
/// thread), always.
class IAskpassPromptSink
{
  public:
    virtual ~IAskpassPromptSink() = default;

    /// Puts a prompt in front of the user.
    ///
    /// The reply may be given at once or later, from the GUI thread, and must be given at most once; a
    /// reply given after withdraw() or after the broker's session is gone is dropped.
    /// @param promptId Names this prompt; withdraw() refers to it by it.
    /// @param kind What the prompt asks of the user.
    /// @param target What the login is to, `ssh://user@host`.
    /// @param prompt The prompt, verbatim from ssh.
    /// @param reply Told how the prompt ended. For a Notice nobody replies: the notice stays until
    /// withdraw().
    virtual void ask(AskpassPromptId promptId,
                     AskpassPromptKind kind,
                     QString target,
                     QString prompt,
                     std::function<void(AskpassReply)> reply) = 0;

    /// The helper process went away with its prompt unanswered (ssh killed it once the user touched the
    /// key, or the attempt ended): take that prompt down.
    /// @param promptId The prompt whose helper went away; one already answered or unknown is ignored.
    virtual void withdraw(AskpassPromptId promptId) = 0;
};

} // namespace contour::askpass
