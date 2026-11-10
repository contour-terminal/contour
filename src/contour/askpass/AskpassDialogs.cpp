// SPDX-License-Identifier: Apache-2.0
#include <contour/askpass/AskpassDialogs.hpp>

#include <QtCore/QTimer>
#include <QtWidgets/QDialog>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMessageBox>

#include <algorithm>
#include <memory>
#include <utility>

namespace contour::askpass
{

namespace
{
    /// Asks a yes-or-no question.
    /// @return The dialog, not yet shown.
    [[nodiscard]] std::unique_ptr<QMessageBox> questionBox(QString const& title, QString const& text)
    {
        auto box = std::make_unique<QMessageBox>(
            QMessageBox::Question, title, text, QMessageBox::Yes | QMessageBox::No);
        box->setDefaultButton(QMessageBox::No);
        return box;
    }
} // namespace

DialogAskpassPromptSink::~DialogAskpassPromptSink()
{
    for (auto const& prompt: std::exchange(_prompts, {}))
        if (prompt.dialog)
            prompt.dialog->reject();
}

void DialogAskpassPromptSink::forget(AskpassPromptId id)
{
    std::erase_if(_prompts, [id](Prompt const& each) { return each.id == id; });
}

void DialogAskpassPromptSink::ask(AskpassPromptId id,
                                  AskpassPromptKind kind,
                                  QString target,
                                  QString prompt,
                                  std::function<void(AskpassReply)> reply)
{
    _prompts.push_back(Prompt { .id = id, .dialog = {}, .shown = Prompt::Shown::Pending });
    // Out of the socket's readyRead slot first: the modal dialogs below run a loop of their own.
    QTimer::singleShot(
        0,
        this,
        [this, id, kind, target = std::move(target), prompt = std::move(prompt), reply = std::move(reply)] {
            auto const found = std::ranges::find(_prompts, id, &Prompt::id);
            // Withdrawn before it was even shown: its helper is gone, nobody waits for a reply.
            if (found == _prompts.end())
                return;
            auto const title = QObject::tr("SSH login to %1").arg(target);
            auto const shown = [this, id](QDialog* dialog) {
                if (auto const each = std::ranges::find(_prompts, id, &Prompt::id); each != _prompts.end())
                {
                    each->dialog = dialog;
                    each->shown = Prompt::Shown::Shown;
                }
            };
            switch (kind)
            {
                case AskpassPromptKind::Secret: {
                    auto dialog = QInputDialog { nullptr };
                    dialog.setWindowTitle(title);
                    dialog.setLabelText(prompt.trimmed());
                    dialog.setTextEchoMode(QLineEdit::Password);
                    shown(&dialog);
                    auto const accepted = dialog.exec() == QDialog::Accepted;
                    auto answer = dialog.textValue().toStdString();
                    forget(id);
                    reply(accepted ? AskpassReply { AskpassOutcome::Answered, answer } : AskpassReply {});
                    wipe(answer);
                    break;
                }
                case AskpassPromptKind::HostKey: {
                    auto box = questionBox(title, prompt.trimmed());
                    shown(box.get());
                    auto const yes = box->exec() == QMessageBox::Yes;
                    forget(id);
                    reply(AskpassReply { AskpassOutcome::Answered, yes ? "yes" : "no" });
                    break;
                }
                case AskpassPromptKind::Confirm: {
                    auto box = questionBox(title, prompt.trimmed());
                    shown(box.get());
                    auto const yes = box->exec() == QMessageBox::Yes;
                    forget(id);
                    reply(yes ? AskpassReply { AskpassOutcome::Answered, {} } : AskpassReply {});
                    break;
                }
                case AskpassPromptKind::Notice: {
                    // Nobody replies to a notice: ssh takes the helper down when it is done with it.
                    auto* const box = new QMessageBox(
                        QMessageBox::Information, title, prompt.trimmed(), QMessageBox::NoButton);
                    box->setAttribute(Qt::WA_DeleteOnClose);
                    shown(box);
                    box->show();
                    break;
                }
            }
        });
}

void DialogAskpassPromptSink::withdraw(AskpassPromptId promptId)
{
    auto const found = std::ranges::find(_prompts, promptId, &Prompt::id);
    if (found == _prompts.end())
        return;
    auto const prompt = *found;
    _prompts.erase(found);
    // One not shown yet is just never shown; one on screen is taken down.
    if (prompt.dialog)
        prompt.dialog->reject();
}

} // namespace contour::askpass
