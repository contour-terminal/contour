// SPDX-License-Identifier: Apache-2.0
#include <vtextension/ExtensionConsent.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <utility>

#include <vtextension/Utf8.hpp>

namespace vtextension
{

namespace
{
    /// How one level of an extension's log reads in its host's.
    struct LogLevelName
    {
        LogLevel level;
        std::string_view name;
    };

    constexpr auto LogLevelNames = std::array {
        LogLevelName { .level = LogLevel::Trace, .name = "trace" },
        LogLevelName { .level = LogLevel::Debug, .name = "debug" },
        LogLevelName { .level = LogLevel::Info, .name = "info" },
        LogLevelName { .level = LogLevel::Warn, .name = "warning" },
        LogLevelName { .level = LogLevel::Error, .name = "error" },
    };

    /// A range of code points, both ends included.
    struct CodePointRange
    {
        char32_t first;
        char32_t last;
    };

    /// What neutralizedForDisplay() shows as code points: what renders as nothing or as something else,
    /// and what reorders or breaks the text around it.
    constexpr auto DisguisingCodePoints = std::array {
        CodePointRange { .first = 0x0000, .last = 0x001F },   // C0 controls
        CodePointRange { .first = 0x007F, .last = 0x009F },   // DEL, C1 controls
        CodePointRange { .first = 0x00AD, .last = 0x00AD },   // soft hyphen
        CodePointRange { .first = 0x061C, .last = 0x061C },   // Arabic letter mark
        CodePointRange { .first = 0x180E, .last = 0x180E },   // Mongolian vowel separator
        CodePointRange { .first = 0x200B, .last = 0x200F },   // zero-width space/joiners, LRM, RLM
        CodePointRange { .first = 0x2028, .last = 0x202E },   // line/paragraph separators, embeddings
        CodePointRange { .first = 0x2060, .last = 0x2064 },   // word joiner, invisible operators
        CodePointRange { .first = 0x2066, .last = 0x206F },   // isolates, deprecated format characters
        CodePointRange { .first = 0xFEFF, .last = 0xFEFF },   // zero-width no-break space
        CodePointRange { .first = 0xFFF9, .last = 0xFFFB },   // interlinear annotation
        CodePointRange { .first = 0xE0000, .last = 0xE007F }, // tag characters
    };

    [[nodiscard]] bool disguises(char32_t codePoint) noexcept
    {
        return std::ranges::any_of(DisguisingCodePoints, [codePoint](CodePointRange const& range) {
            return codePoint >= range.first && codePoint <= range.last;
        });
    }

    [[nodiscard]] bool needsQuotes(std::string_view element) noexcept
    {
        return element.empty() || element.contains(' ');
    }
} // namespace

std::string neutralizedForDisplay(std::string_view text)
{
    auto shown = std::string {};
    shown.reserve(text.size());
    while (!text.empty())
    {
        auto const decoded = decodeUtf8(text);
        if (!decoded)
        {
            shown += std::format("<0x{:02X}>", static_cast<unsigned char>(text.front()));
            text.remove_prefix(1);
            continue;
        }
        auto const [codePoint, length] = *decoded;
        if (disguises(codePoint))
            shown += std::format("<U+{:04X}>", static_cast<uint32_t>(codePoint));
        else
            shown += text.substr(0, length);
        text.remove_prefix(length);
    }
    return shown;
}

std::string extensionLogLine(std::string_view name, LogLevel level, std::string_view message)
{
    auto const known = std::ranges::find(LogLevelNames, level, &LogLevelName::level);
    return std::format("{} [{}]: {}",
                       name,
                       known != LogLevelNames.end() ? known->name : std::string_view { "?" },
                       neutralizedForDisplay(message));
}

std::string displayedCommandLine(DeclaredCommand const& command)
{
    auto line = std::string {};
    for (auto const& element: command.argvTemplate)
    {
        if (!line.empty())
            line += ' ';
        auto const shown = neutralizedForDisplay(element);
        line += needsQuotes(element) ? std::format("'{}'", shown) : shown;
    }
    return line;
}

std::string consentText(ExtensionConsentRequest const& request)
{
    // The first line holds nothing the guest chose: a dialog that guesses whether its text is rich text reads
    // only up to the first line break, and must never take the guest's `<b>` for markup.
    auto text = std::format("{}\n\nExtension: \"{}\"\nModule: {}\n\n"
                            "It may run only these, exactly as written ({{name}} is filled in by the "
                            "extension):\n",
                            ConsentHeadline,
                            neutralizedForDisplay(request.manifest.name),
                            neutralizedForDisplay(request.hostId));
    for (auto const& command: request.manifest.commands)
        text +=
            std::format("\n    {}: {}", neutralizedForDisplay(command.name), displayedCommandLine(command));
    text += "\n\nAllow it? The answer is remembered until the extension's manifest changes.";
    return text;
}

ConsentPrompt ExtensionConsentQueue::offer(ExtensionConsentRequest request, IGrantStore& answers)
{
    if (answers.lookup(request.hostId, request.manifest.digest) != GrantState::Pending)
        return ConsentPrompt::None;
    auto const sameQuestion = [&](Waiting const& waiting) {
        return waiting.answers == &answers && waiting.request.hostId == request.hostId
               && waiting.request.manifest.digest == request.manifest.digest;
    };
    if (std::ranges::any_of(_waiting, sameQuestion))
        return ConsentPrompt::None;
    _waiting.push_back(Waiting { .request = std::move(request), .answers = &answers });
    return _waiting.size() == 1 ? ConsentPrompt::Due : ConsentPrompt::None;
}

ConsentPrompt ExtensionConsentQueue::answer(ConsentAnswer answer)
{
    if (_waiting.empty())
        return ConsentPrompt::None;
    auto const& [asked, answers] = _waiting.front();
    switch (answer)
    {
        case ConsentAnswer::Dismissed: break;
        case ConsentAnswer::Denied:
            answers->record(asked.hostId, asked.manifest.digest, GrantState::Denied);
            break;
        case ConsentAnswer::Allowed:
            answers->record(asked.hostId, asked.manifest.digest, GrantState::Granted);
            break;
    }
    _waiting.pop_front();
    dropAnswered();
    return _waiting.empty() ? ConsentPrompt::None : ConsentPrompt::Due;
}

void ExtensionConsentQueue::dropAnswered()
{
    while (!_waiting.empty()
           && _waiting.front().answers->lookup(_waiting.front().request.hostId,
                                               _waiting.front().request.manifest.digest)
                  != GrantState::Pending)
        _waiting.pop_front();
}

} // namespace vtextension
