// SPDX-License-Identifier: Apache-2.0
#include <contour/window/PromptHost.hpp>

#include <QtCore/QCoreApplication>

#include <algorithm>
#include <ranges>
#include <utility>

namespace contour::window
{

PromptKindRow const& promptKindRow(askpass::AskpassPromptKind kind) noexcept
{
    return PromptKindRows[static_cast<std::size_t>(kind)];
}

PromptHost::PromptHost(std::shared_ptr<askpass::AskpassPromptRouter> router,
                       askpass::AskpassRouteId route,
                       QObject* parent):
    QObject(parent), _router { std::move(router) }, _route { route }
{
    _router->attach(_route, *this);
}

PromptHost::~PromptHost()
{
    // The router cancels what this host showed: the window that would have shown it is gone.
    _router->detach(_route);
}

PromptHost::Open const* PromptHost::current() const
{
    auto const isQuestion = [](Open const& each) {
        return promptKindRow(each.kind).answer != PromptKindRow::Answer::NoticeOnly;
    };
    if (auto const question = std::ranges::find_if(_open, isQuestion); question != _open.end())
        return &*question;
    return _open.empty() ? nullptr : &_open.front();
}

QVariantMap PromptHost::pendingPrompt() const
{
    auto const* const open = current();
    if (open == nullptr)
        return {};
    auto const& row = promptKindRow(open->kind);
    auto const translated = [](char const* text) {
        return QCoreApplication::translate("PromptHost", text);
    };
    return QVariantMap {
        { QStringLiteral("promptId"), static_cast<int>(open->id) },
        { QStringLiteral("heading"), translated(row.heading).arg(open->target) },
        { QStringLiteral("target"), open->target },
        { QStringLiteral("text"), open->text.trimmed() },
        { QStringLiteral("isVerbatim"), row.text == PromptKindRow::Text::Verbatim },
        { QStringLiteral("isSecret"), row.answer == PromptKindRow::Answer::TypedSecret },
        { QStringLiteral("acceptText"), translated(row.acceptText) },
        { QStringLiteral("rejectText"), translated(row.rejectText) },
        { QStringLiteral("acceptOutcome"), static_cast<int>(row.acceptOutcome) },
        { QStringLiteral("acceptAnswer"), QString::fromLatin1(row.acceptAnswer) },
        { QStringLiteral("rejectOutcome"), static_cast<int>(row.rejectOutcome) },
        { QStringLiteral("rejectAnswer"), QString::fromLatin1(row.rejectAnswer) },
    };
}

void PromptHost::ask(askpass::AskpassPromptId promptId,
                     askpass::AskpassPromptKind kind,
                     QString target,
                     QString prompt,
                     std::function<void(askpass::AskpassReply)> reply)
{
    _open.push_back(Open { .id = promptId,
                           .kind = kind,
                           .target = std::move(target),
                           .text = std::move(prompt),
                           .reply = std::move(reply) });
    emit pendingPromptChanged();
}

void PromptHost::withdraw(askpass::AskpassPromptId promptId)
{
    if (std::erase_if(_open, [promptId](Open const& each) { return each.id == promptId; }) != 0)
        emit pendingPromptChanged();
}

void PromptHost::answerPrompt(int promptId, int outcome, QString const& answer)
{
    auto const found = std::ranges::find(_open, static_cast<askpass::AskpassPromptId>(promptId), &Open::id);
    if (found == _open.end())
        return;
    auto reply = std::move(found->reply);
    _open.erase(found);
    auto given =
        askpass::AskpassReply { .outcome = outcome == static_cast<int>(askpass::AskpassOutcome::Answered)
                                               ? askpass::AskpassOutcome::Answered
                                               : askpass::AskpassOutcome::Cancelled,
                                .answer = {} };
    if (given.outcome == askpass::AskpassOutcome::Answered)
    {
        // Straight from the field's text into a Secret; the intermediate encoding is wiped as it goes.
        auto utf8 = answer.toUtf8();
        given.answer =
            askpass::Secret { std::string_view { utf8.constData(), static_cast<std::size_t>(utf8.size()) } };
        utf8.fill('\0');
    }
    // What the QString held, as far as this copy of it goes.
    answer.fill(QChar { u'\0' });
    emit pendingPromptChanged();
    if (reply)
        reply(std::move(given));
}

} // namespace contour::window
