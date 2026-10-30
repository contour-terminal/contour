// SPDX-License-Identifier: Apache-2.0
#include <vtextension/ExtensionConsent.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <utility>

#include <vtcontrol/Catalog.hpp>
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

ConsentQuestion consentQuestion(ExtensionConsentRequest const& request)
{
    auto const& manifest = request.manifest;
    auto const name = neutralizedForDisplay(manifest.name);
    auto question = ConsentQuestion {
        .title = manifest.control.empty() ? std::format("“{}” wants to run commands", name)
                                          : std::format("“{}” wants to control Contour", name),
        .lines = {},
    };
    auto const line = [&question](std::string text, vtcontrol::ConsentWeight weight) {
        question.lines.push_back(ConsentLine { .text = std::move(text), .weight = weight });
    };
    line(std::format("Module: {}", neutralizedForDisplay(request.hostId)),
         vtcontrol::ConsentWeight::Observes);
    // A manifest of control permissions alone has no commands to list.
    if (!manifest.commands.empty() || manifest.control.empty())
    {
        line("It may run only these, exactly as written ({name} is filled in by the extension):",
             vtcontrol::ConsentWeight::Observes);
        for (auto const& command: manifest.commands)
            line(std::format("{}: {}", neutralizedForDisplay(command.name), displayedCommandLine(command)),
                 vtcontrol::ConsentWeight::Observes);
    }
    for (auto const permission: manifest.control)
    {
        auto const weight = vtcontrol::consentWeightOf(permission);
        line(std::format("May {}{}.",
                         vtcontrol::consentPhraseOf(permission),
                         weight == vtcontrol::ConsentWeight::CanRunCommands ? " (can run commands)" : ""),
             weight);
    }
    return question;
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
