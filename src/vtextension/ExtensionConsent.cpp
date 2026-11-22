// SPDX-License-Identifier: Apache-2.0
#include <vtextension/ExtensionConsent.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <span>
#include <string>
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
        if (disguisesText(codePoint))
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

namespace
{
    /// @return @p argv as displayedCommandLine() shows a command's.
    [[nodiscard]] std::string displayedArgv(std::span<std::string const> argv)
    {
        auto line = std::string {};
        for (auto const& element: argv)
        {
            if (!line.empty())
                line += ' ';
            auto const shown = neutralizedForDisplay(element);
            line += needsQuotes(element) ? std::format("'{}'", shown) : shown;
        }
        return line;
    }

    /// How the daemon question names a command's timeout class.
    struct TimeoutWord
    {
        CommandTimeout timeout;
        std::string_view word;
    };

    inline constexpr auto TimeoutWords = std::array {
        TimeoutWord { .timeout = CommandTimeout::Short, .word = "short" },
        TimeoutWord { .timeout = CommandTimeout::Long, .word = "long" },
    };

    /// @return The word for @p timeout.
    [[nodiscard]] std::string_view timeoutWordOf(CommandTimeout timeout) noexcept
    {
        auto const row = std::ranges::find(TimeoutWords, timeout, &TimeoutWord::timeout);
        return row != TimeoutWords.end() ? row->word : std::string_view { "?" };
    }

    /// @return How a root's directory line names it: its label, and that it is created when declared so.
    [[nodiscard]] std::string directoryLabelOf(Manifest const& manifest, std::string_view label)
    {
        auto const declared = std::ranges::find(manifest.roots, label, &RootDecl::label);
        auto const created =
            declared != manifest.roots.end() && declared->creation == RootCreation::CreateIfMissing;
        return std::format("{}{}", neutralizedForDisplay(label), created ? ", created if missing" : "");
    }
} // namespace

std::string displayedCommandLine(DeclaredCommand const& command)
{
    return displayedArgv(command.argvTemplate);
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

ConsentQuestion daemonConsentQuestion(DaemonConsentRequest const& request)
{
    using vtcontrol::ConsentWeight;
    auto const& manifest = request.manifest;
    auto question = ConsentQuestion {
        .title = std::format("“{}” wants to run in the background", neutralizedForDisplay(manifest.name)),
        .lines = {},
    };
    auto const line = [&question](std::string text, ConsentWeight weight) {
        question.lines.push_back(ConsentLine { .text = std::move(text), .weight = weight });
    };
    line(std::format("Module: {}", neutralizedForDisplay(request.hostId)), ConsentWeight::Observes);
    for (auto const& root: request.roots)
        for (auto const& path: root.paths)
            line(std::format("Directory ({}): {}",
                             directoryLabelOf(manifest, root.label),
                             neutralizedForDisplay(path)),
                 ConsentWeight::Observes);
    // What a program runs with steers what it runs: every variable is shown, beneath what it is set for.
    auto const environment = [&line](std::span<EnvironmentEntry const> entries) {
        for (auto const& [name, value]: entries)
            line(std::format("  {}={}", neutralizedForDisplay(name), neutralizedForDisplay(value)),
                 ConsentWeight::CanRunCommands);
    };
    if (!manifest.commands.empty())
        line("It may run only these, which can reach the network as you:", ConsentWeight::Observes);
    if (!manifest.commands.empty() && !manifest.commandEnvironment.empty())
    {
        line("Each of them with:", ConsentWeight::CanRunCommands);
        environment(manifest.commandEnvironment);
    }
    for (auto const& command: manifest.commands)
    {
        line(std::format("{} ({}, in {}): {}",
                         neutralizedForDisplay(command.name),
                         timeoutWordOf(command.timeout),
                         neutralizedForDisplay(command.root),
                         displayedArgv(command.argvTemplate)),
             ConsentWeight::CanRunCommands);
        environment(command.environment);
    }
    for (auto const& pane: manifest.panes)
    {
        line(std::format("It may open tabs running: {} (in {}): {}",
                         neutralizedForDisplay(pane.name),
                         neutralizedForDisplay(pane.root),
                         displayedArgv(pane.argvTemplate)),
             ConsentWeight::CanRunCommands);
        environment(pane.environment);
    }
    if (manifest.paneInput == PaneInput::Typed)
        line("It may type text you write into those tabs, or press a number key you choose, when you send it "
             "from its own tab",
             ConsentWeight::CanRunCommands);
    for (auto const& root: manifest.readableRoots)
        line(std::format("It may read files in: {}", neutralizedForDisplay(root)), ConsentWeight::Observes);
    if (manifest.servedScheme)
        line(std::format("It shows its own tab: {}://", neutralizedForDisplay(*manifest.servedScheme)),
             ConsentWeight::Observes);
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
