// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A window's host for ssh's prompts: the sink of the askpass broker that belongs to one window, and the
/// model its AskpassDialog shows.

#include <contour/askpass/AskpassPromptRouter.hpp>
#include <contour/askpass/AskpassPromptSink.hpp>
#include <contour/askpass/AskpassProtocol.hpp>

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QVariantMap>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <ranges>
#include <vector>

namespace contour::window
{

/// What the dialog is made of for one kind of prompt: the data that decides, so that a new kind is a row.
struct PromptKindRow
{
    /// How the user answers.
    enum class Answer : std::uint8_t
    {
        TypedSecret = 0, ///< A masked field; the answer is what was typed.
        Question,        ///< Two buttons; the answer is the button's.
        NoticeOnly,      ///< Nothing to answer; the user may only cancel the login.
    };

    askpass::AskpassPromptKind kind;
    Answer answer;
    /// How ssh's text is shown.
    enum class Text : std::uint8_t
    {
        Plain = 0,
        Verbatim, ///< As it is, selectable, in a monospaced block: a fingerprint is compared by eye.
    };
    Text text;
    char const* heading;    ///< Translated when shown.
    char const* acceptText; ///< Empty: no accept button.
    char const* rejectText;
    askpass::AskpassOutcome acceptOutcome;
    char const* acceptAnswer; ///< What ssh is told for the accept button; a typed secret ignores it.
    askpass::AskpassOutcome rejectOutcome;
    char const* rejectAnswer;
    /// A sentence under the heading, in the dialog's ordinary text and not the prompt's block, that says what
    /// to do with the prompt; translated when shown. Empty: none.
    char const* hint;
};

/// The rows, one per askpass::AskpassPromptKind, in its order.
inline constexpr auto PromptKindRows = std::array {
    PromptKindRow { askpass::AskpassPromptKind::Secret,
                    PromptKindRow::Answer::TypedSecret,
                    PromptKindRow::Text::Plain,
                    QT_TRANSLATE_NOOP("PromptHost", "Log in to %1"),
                    QT_TRANSLATE_NOOP("PromptHost", "OK"),
                    QT_TRANSLATE_NOOP("PromptHost", "Cancel"),
                    askpass::AskpassOutcome::Answered,
                    "",
                    askpass::AskpassOutcome::Cancelled,
                    "",
                    "" },
    PromptKindRow { askpass::AskpassPromptKind::Confirm,
                    PromptKindRow::Answer::Question,
                    PromptKindRow::Text::Verbatim,
                    QT_TRANSLATE_NOOP("PromptHost", "ssh asks about %1"),
                    QT_TRANSLATE_NOOP("PromptHost", "Yes"),
                    QT_TRANSLATE_NOOP("PromptHost", "No"),
                    askpass::AskpassOutcome::Answered,
                    "",
                    askpass::AskpassOutcome::Cancelled,
                    "",
                    "" },
    PromptKindRow { askpass::AskpassPromptKind::Notice,
                    PromptKindRow::Answer::NoticeOnly,
                    PromptKindRow::Text::Plain,
                    QT_TRANSLATE_NOOP("PromptHost", "Waiting for %1"),
                    "",
                    QT_TRANSLATE_NOOP("PromptHost", "Cancel"),
                    askpass::AskpassOutcome::Answered,
                    "",
                    askpass::AskpassOutcome::Cancelled,
                    "",
                    "" },
    PromptKindRow { askpass::AskpassPromptKind::HostKey,
                    PromptKindRow::Answer::Question,
                    PromptKindRow::Text::Verbatim,
                    QT_TRANSLATE_NOOP("PromptHost", "Trust the host key of %1?"),
                    QT_TRANSLATE_NOOP("PromptHost", "Yes"),
                    QT_TRANSLATE_NOOP("PromptHost", "No"),
                    askpass::AskpassOutcome::Answered,
                    "yes",
                    askpass::AskpassOutcome::Answered,
                    "no",
                    "" },
    PromptKindRow { askpass::AskpassPromptKind::HostKeyWarning,
                    PromptKindRow::Answer::Question,
                    PromptKindRow::Text::Verbatim,
                    QT_TRANSLATE_NOOP("PromptHost", "Warning: ssh doubts the host key of %1"),
                    QT_TRANSLATE_NOOP("PromptHost", "Yes"),
                    QT_TRANSLATE_NOOP("PromptHost", "No"),
                    askpass::AskpassOutcome::Answered,
                    "yes",
                    askpass::AskpassOutcome::Answered,
                    "no",
                    "" },
    PromptKindRow { askpass::AskpassPromptKind::HostKeyUpdate,
                    PromptKindRow::Answer::Question,
                    PromptKindRow::Text::Verbatim,
                    QT_TRANSLATE_NOOP("PromptHost", "Take the new host keys of %1?"),
                    QT_TRANSLATE_NOOP("PromptHost", "Yes"),
                    QT_TRANSLATE_NOOP("PromptHost", "No"),
                    askpass::AskpassOutcome::Answered,
                    "yes",
                    askpass::AskpassOutcome::Answered,
                    "no",
                    "" },
    PromptKindRow { askpass::AskpassPromptKind::Question,
                    PromptKindRow::Answer::Question,
                    PromptKindRow::Text::Verbatim,
                    QT_TRANSLATE_NOOP("PromptHost", "ssh asks about %1"),
                    QT_TRANSLATE_NOOP("PromptHost", "Yes"),
                    QT_TRANSLATE_NOOP("PromptHost", "No"),
                    askpass::AskpassOutcome::Answered,
                    "yes",
                    askpass::AskpassOutcome::Answered,
                    "no",
                    "" },
    // Not ssh's: the application asks whether to pin a daemon's fingerprint. Verbatim, so that the
    // fingerprint is selectable and compared by eye, and Cancel (the reject button, the default) answers
    // nothing.
    PromptKindRow {
        askpass::AskpassPromptKind::PinConfirm,
        PromptKindRow::Answer::Question,
        PromptKindRow::Text::Verbatim,
        QT_TRANSLATE_NOOP("PromptHost", "Trust the daemon at %1?"),
        QT_TRANSLATE_NOOP("PromptHost", "Trust"),
        QT_TRANSLATE_NOOP("PromptHost", "Cancel"),
        askpass::AskpassOutcome::Answered,
        "yes",
        askpass::AskpassOutcome::Cancelled,
        "",
        QT_TRANSLATE_NOOP("PromptHost", "Compare with `contour daemon status` on that machine.") },
};

namespace detail
{
    [[nodiscard]] constexpr bool promptKindRowsInOrder() noexcept
    {
        for (auto const index: std::views::iota(std::size_t { 0 }, PromptKindRows.size()))
            if (static_cast<std::size_t>(PromptKindRows[index].kind) != index)
                return false;
        return true;
    }
} // namespace detail

static_assert(detail::promptKindRowsInOrder(), "PromptKindRows must list the prompt kinds in order");
static_assert(PromptKindRows.size() == static_cast<std::size_t>(askpass::AskpassPromptKind::PinConfirm) + 1,
              "PromptKindRows must have a row for every prompt kind");

/// @param kind A prompt kind.
/// @return Its row.
[[nodiscard]] PromptKindRow const& promptKindRow(askpass::AskpassPromptKind kind) noexcept;

/// Shows the prompts that belong to one window, one at a time, in the window's AskpassDialog.
///
/// The host attaches itself to the application's router for the window's route and detaches again when it is
/// destroyed, so a prompt waits for a window rather than being lost when none is there yet. The oldest
/// prompt that asks something is shown first, a notice (which asks nothing) only when no question is open.
class PromptHost final: public QObject, public askpass::IAskpassPromptSink
{
    Q_OBJECT
    /// The prompt shown now as a map -- `{}` while none is. Its keys are the dialog's: promptId,
    /// heading, target, text, isVerbatim, isSecret, acceptText, rejectText, acceptOutcome, acceptAnswer,
    /// rejectOutcome, rejectAnswer.
    Q_PROPERTY(QVariantMap pendingPrompt READ pendingPrompt NOTIFY pendingPromptChanged)

  public:
    /// @param router Where prompts are sent to this host; shared with the broker.
    /// @param route The window this host belongs to.
    /// @param parent The window's controller.
    PromptHost(std::shared_ptr<askpass::AskpassPromptRouter> router,
               askpass::AskpassRouteId route,
               QObject* parent = nullptr);
    ~PromptHost() override;

    PromptHost(PromptHost const&) = delete;
    PromptHost& operator=(PromptHost const&) = delete;
    PromptHost(PromptHost&&) = delete;
    PromptHost& operator=(PromptHost&&) = delete;

    /// @return The prompt shown now, or an empty map.
    [[nodiscard]] QVariantMap pendingPrompt() const;

    /// The user answered the prompt shown. Ignored for a prompt that is not open any more (withdrawn, or
    /// answered already), so a doubled answer cannot answer the next prompt.
    /// @param promptId The prompt answered, as `pendingPrompt` named it.
    /// @param outcome An askpass::AskpassOutcome.
    /// @param answer What was typed, for a secret; the button's answer otherwise. Not kept.
    Q_INVOKABLE void answerPrompt(int promptId, int outcome, QString const& answer);

    /// @copydoc askpass::IAskpassPromptSink::ask
    void ask(askpass::AskpassPromptId promptId,
             askpass::AskpassPromptKind kind,
             QString target,
             QString prompt,
             std::function<void(askpass::AskpassReply)> reply) override;

    /// @copydoc askpass::IAskpassPromptSink::withdraw
    void withdraw(askpass::AskpassPromptId promptId) override;

  signals:
    void pendingPromptChanged();

  private:
    struct Open
    {
        askpass::AskpassPromptId id = 0;
        askpass::AskpassPromptKind kind = askpass::AskpassPromptKind::Secret;
        QString target;
        QString text;
        std::function<void(askpass::AskpassReply)> reply;
    };

    /// @return The prompt to show: the oldest question, else the oldest notice; none when nothing is open.
    [[nodiscard]] Open const* current() const;

    std::shared_ptr<askpass::AskpassPromptRouter> _router;
    askpass::AskpassRouteId _route;
    std::vector<Open> _open; ///< In the order asked.
};

} // namespace contour::window
