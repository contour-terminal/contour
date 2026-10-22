// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>

#include <vtextension/CommandPolicy.hpp>
#include <vtextension/IHostServices.hpp>
#include <vtextension/Manifest.hpp>

namespace vtextension
{

/// Makes text safe to show the user where it must read as exactly what it is: a permission prompt, a log
/// line. Every character that renders as nothing, or as something else, or that reorders or breaks the text
/// around it -- controls, bidirectional embeddings, overrides and isolates, zero-width characters, the line
/// and paragraph separators U+2028/U+2029, tag characters -- is shown as its code point instead, `<U+202E>`;
/// a byte that is not UTF-8 as `<0xFF>`. Everything else is kept as it is.
/// @param text Text a guest chose.
/// @return What to show.
[[nodiscard]] std::string neutralizedForDisplay(std::string_view text);

/// What an extension logged, as one line of its host's log: its name, the level, and the message -- the
/// guest's own text, so neutralized: a log shown in a terminal must not carry the guest's escape sequences.
/// @param name What the configuration calls the extension.
/// @param level How loud the message is.
/// @param message What the extension logged.
/// @return The line.
[[nodiscard]] std::string extensionLogLine(std::string_view name, LogLevel level, std::string_view message);

/// One command of a manifest as the prompt shows it: its argv template, every element verbatim (and
/// neutralized), separated by spaces. An element that is empty or holds a space is shown in single quotes,
/// so that the elements can still be told apart.
/// @param command The declared command.
/// @return The command line.
[[nodiscard]] std::string displayedCommandLine(DeclaredCommand const& command);

/// An extension asking for permission to run the commands its manifest declares.
struct ExtensionConsentRequest
{
    std::string hostId; ///< Which installed extension asks: its config key and its module.
    Manifest manifest;  ///< What it declares, and the digest the answer is bound to.
};

/// The first line of every consent prompt: fixed, and free of anything a guest chose.
inline constexpr auto ConsentHeadline = std::string_view { "A status-line extension asks to run commands." };

/// @param request A request.
/// @return What the prompt asking it says: who asks, from which module, and every command it may run, each
///         verbatim. Plain text whatever the guest declares: its first line is ConsentHeadline, so that a
///         dialog that guesses the format (Qt::mightBeRichText reads up to the first line break) finds no
///         markup there.
[[nodiscard]] std::string consentText(ExtensionConsentRequest const& request);

/// What the user answered.
enum class ConsentAnswer : uint8_t
{
    Dismissed, ///< Closed the prompt: nothing is recorded, and the question is asked again in a new pane.
    Denied,    ///< Refused; remembered.
    Allowed,   ///< Approved; remembered.
};

/// Whether a prompt must be shown now.
enum class ConsentPrompt : uint8_t
{
    None, ///< Nothing to show, or the prompt already showing stays as it is.
    Due,  ///< current() changed: show it.
};

/// The permission questions one session has to ask, one at a time, in the order they came.
///
/// Each question is asked for the grant store its answer is recorded in: the application's, for an
/// extension the session runs itself, or a daemon's, for one a daemon-hosted session runs there. A question
/// already answered in its store, or already waiting, is not asked again, so two extensions of the same
/// installation or a repeated request show one prompt. GUI thread only.
class ExtensionConsentQueue
{
  public:
    /// Queues a question.
    /// @param request The question.
    /// @param answers Where its answer is recorded and looked up; it must outlive the queue.
    /// @return Due if it is the one to show now.
    [[nodiscard]] ConsentPrompt offer(ExtensionConsentRequest request, IGrantStore& answers);

    /// @return The question being shown; null if none is.
    [[nodiscard]] ExtensionConsentRequest const* current() const noexcept
    {
        return _waiting.empty() ? nullptr : &_waiting.front().request;
    }

    /// Records the answer to current(), and moves on to the next question. Does nothing without one.
    /// @param answer The answer.
    /// @return Due if another question is now to be shown.
    [[nodiscard]] ConsentPrompt answer(ConsentAnswer answer);

  private:
    /// Drops the front questions answered meanwhile (through another session's prompt).
    void dropAnswered();

    /// A question, and where its answer goes.
    struct Waiting
    {
        ExtensionConsentRequest request;
        IGrantStore* answers = nullptr;
    };

    std::deque<Waiting> _waiting;
};

} // namespace vtextension
