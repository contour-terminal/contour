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

/// Why buildPrompt refused a prompt.
enum class PromptError : uint8_t
{
    StartsWithDash,     ///< The prompt starts with '-', so the agent could read it as an option.
    UrlNotHttps,        ///< The URL is not `https://…`, or holds a byte outside printable ASCII or a quote.
    TooLong,            ///< The prompt exceeds MaxTextBytes.
    NoteStartsWithDash, ///< singleLineNote refused the note: it starts with '-'.
    NoteTooLong,        ///< singleLineNote refused the note: it exceeds MaxNoteChars.
    TemplateNotText,    ///< The template holds a NUL, which no prompt may carry.
};

/// The agent's first prompt: the task's template with the item's URL, and the user's note on a line of its
/// own. A title or a body is never part of it.
/// @param promptTemplate The task's prompt, with `{url}` standing for the item's URL wherever it occurs.
/// @param url The item's web page, as the forge reported it; it must be `https://` and one word without a
///        quote, so that it cannot end the argument it is placed in.
/// @param note The user's note, sanitised with singleLineNote; one that sanitises to nothing adds no line.
/// @return The prompt, which argumentAccepts(ArgumentKind::Text, …) accepts, or why it was refused.
[[nodiscard]] std::expected<std::string, PromptError> buildPrompt(std::string_view promptTemplate,
                                                                  std::string_view url,
                                                                  std::optional<std::string_view> note);

} // namespace orchestrator
