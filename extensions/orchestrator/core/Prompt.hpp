// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace orchestrator
{

/// What a task's prompt template writes where the item's URL goes.
inline constexpr std::string_view UrlPlaceholder = "{url}";

/// What a task's prompt template writes where the item's key goes: a Jira key, or a forge number.
inline constexpr std::string_view KeyPlaceholder = "{key}";

/// What a topic task's prompt template writes where the topic's text goes.
inline constexpr std::string_view TopicPlaceholder = "{topic}";

/// Why buildPrompt refused a prompt.
enum class PromptError : uint8_t
{
    StartsWithDash, ///< The prompt starts with '-', so the agent could read it as an option.
    UrlNotHttps,    ///< The URL is not `https://…`, or holds a byte outside printable ASCII or a quote.
    KeyNotAccepted, ///< The template uses `{key}` and the key is empty, or holds a byte other than a letter,
                    ///< digit or '-', or starts with '-'.
    TooLong,        ///< The prompt exceeds MaxTextBytes.
    NoteStartsWithDash, ///< singleLineNote refused the note: it starts with '-'.
    NoteTooLong,        ///< singleLineNote refused the note: it exceeds MaxNoteChars.
    TemplateNotText,    ///< The template holds a NUL, which no prompt may carry.
    TopicEmpty,         ///< buildTopicPrompt's topic sanitises to nothing.
};

/// The agent's first prompt: the task's template with the item's URL and key, and the user's note on a line
/// of its own. A title or a body is never part of it.
/// @param promptTemplate The task's prompt, with `{url}` standing for the item's URL and `{key}` for its key,
///        wherever they occur.
/// @param url The item's web page, as the forge reported it; it must be `https://` and one word without a
///        quote, so that it cannot end the argument it is placed in.
/// @param key The item's key (itemKey()); checked only when the template uses `{key}`.
/// @param note The user's note, sanitised with singleLineNote; one that sanitises to nothing adds no line.
/// @return The prompt, which argumentAccepts(ArgumentKind::Text, …) accepts, or why it was refused.
[[nodiscard]] std::expected<std::string, PromptError> buildPrompt(std::string_view promptTemplate,
                                                                  std::string_view url,
                                                                  std::string_view key,
                                                                  std::optional<std::string_view> note);

/// A topic run's first prompt: the task's template with every `{topic}` replaced by the topic, sanitised to
/// one line (singleLineNote); a template without `{topic}` gets the topic on a line of its own after it.
/// @param promptTemplate The task's prompt.
/// @param topic The topic as the user typed it: their own text, not an item's.
/// @return The prompt, which argumentAccepts(ArgumentKind::Text, …) accepts, or why it was refused.
[[nodiscard]] std::expected<std::string, PromptError> buildTopicPrompt(std::string_view promptTemplate,
                                                                       std::string_view topic);

} // namespace orchestrator
