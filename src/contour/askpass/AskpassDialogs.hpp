// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The application's own answer to ssh's prompts: a dialog of its own per prompt, shown on the GUI thread.
/// It stands in until a window hosts the prompts itself.

#include <contour/askpass/AskpassPromptSink.hpp>

#include <QtCore/QObject>
#include <QtCore/QPointer>

#include <cstdint>
#include <functional>
#include <vector>

class QDialog;

namespace contour::askpass
{

/// Shows each prompt in a modal dialog -- a masked line for a secret, yes and no for a question -- or, for
/// a notice, in a window that stays up, without blocking, until withdraw().
class DialogAskpassPromptSink final: public QObject, public IAskpassPromptSink
{
  public:
    DialogAskpassPromptSink() = default;
    ~DialogAskpassPromptSink() override;

    DialogAskpassPromptSink(DialogAskpassPromptSink const&) = delete;
    DialogAskpassPromptSink& operator=(DialogAskpassPromptSink const&) = delete;
    DialogAskpassPromptSink(DialogAskpassPromptSink&&) = delete;
    DialogAskpassPromptSink& operator=(DialogAskpassPromptSink&&) = delete;

    /// @copydoc IAskpassPromptSink::ask
    void ask(AskpassPromptId promptId,
             AskpassPromptKind kind,
             QString target,
             QString prompt,
             std::function<void(AskpassReply)> reply) override;

    /// @copydoc IAskpassPromptSink::withdraw
    void withdraw(AskpassPromptId promptId) override;

  private:
    /// A prompt asked and not yet taken down.
    struct Prompt
    {
        AskpassPromptId id = 0;
        QPointer<QDialog> dialog; ///< Empty until shown, and again once the dialog is gone.
        enum class Shown : std::uint8_t
        {
            Pending = 0, ///< Asked, its turn on the event loop not yet come.
            Shown,
        } shown = Shown::Pending;
    };

    /// Forgets the prompt @p id.
    void forget(AskpassPromptId id);

    std::vector<Prompt> _prompts; ///< In the order asked; several may be open at once, a notice and a secret.
};

} // namespace contour::askpass
