// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/links/LinkPatterns.hpp>

#include <vtbackend/core/RegexEngine.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <iterator>
#include <ranges>

namespace vtbackend
{

namespace
{
    /// One built-in row: what it finds and how it takes part.
    struct BuiltinPattern
    {
        std::string_view name;
        LinkKind kind;
        LinkActivation activation;
        std::string_view regex;
    };

    // clang-format off
    constexpr auto BuiltinPatterns = std::array {
        BuiltinPattern { "url", LinkKind::Url, LinkActivation::Clickable,
                         R"re(\b(?:(?:https?|ftp|file)://|mailto:)[^\s<>"'`]+)re" },
        // Delimiter-anchored (linear on long word runs, §2.3); the span is group 1, the location groups 2-5.
        BuiltinPattern { "filepath", LinkKind::Path, LinkActivation::Clickable,
                         R"re((?:^|[\s"'`(<\[{=:,])((?:[A-Za-z]:[\\/]|~?[\\/]|\.{1,2}[\\/])?[\w.~-]+(?:[\\/][\w.~-]+)*[\\/]?)(?::(\d+)(?::(\d+))?|\((\d+)(?:,(\d+))?\))?)re" },
        BuiltinPattern { "githash", LinkKind::Custom, LinkActivation::HintOnly, R"re(\b[0-9a-f]{7,40}\b)re" },
        BuiltinPattern { "ipv4", LinkKind::Custom, LinkActivation::HintOnly,
                         R"re(\b\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}(?::\d+)?\b)re" },
        // The full form takes no `:` after it, so no later alternative matches longer from where it starts:
        // libstdc++'s breadth-first executor takes the longest alternative, ECMAScript the first.
        BuiltinPattern { "ipv6", LinkKind::Custom, LinkActivation::HintOnly,
                         R"re((?:\b[0-9a-fA-F]{1,4}(?::[0-9a-fA-F]{1,4}){7}\b(?!:)|\b(?:[0-9a-fA-F]{1,4}:)*[0-9a-fA-F]{1,4}::(?:[0-9a-fA-F]{1,4}:)*[0-9a-fA-F]{1,4}\b|::(?:[0-9a-fA-F]{1,4}:)*[0-9a-fA-F]{1,4}\b|\b(?:[0-9a-fA-F]{1,4}:)+:(?![0-9a-fA-F:])))re" },
    };
    // clang-format on

    /// The built-ins' names, in table order: derived, so that a row added above is named here too.
    constexpr auto BuiltinNames = [] {
        auto names = std::array<std::string_view, BuiltinPatterns.size()> {};
        std::ranges::transform(BuiltinPatterns, names.begin(), &BuiltinPattern::name);
        return names;
    }();

    /// Short inputs on which a pattern that can match the empty string does so somewhere.
    constexpr auto EmptyMatchProbes = std::array<std::string_view, 7> { "", " ", "a", "a b", "/", "1", "-" };

    /// A pattern before compilation: a built-in, then whatever the entries changed.
    struct Draft
    {
        std::string name;
        LinkKind kind;
        LinkActivation activation;
        std::string source;
        std::optional<std::string> urlTemplate {}; ///< As configured; checked once the regex compiled.
    };

    /// What a draft's `url:` becomes, or the problem that disables or warns about it.
    struct CompiledTemplate
    {
        std::optional<LinkUrlTemplate> urlTemplate;
        std::optional<LinkPatternProblem> problem;
    };

    /// Compiles @p draft's `url:` against @p regex: a user pattern's, naming only groups the regex has. A
    /// built-in's is dropped, with a warning: by its name, as githash, ipv4 and ipv6 find custom links too.
    [[nodiscard]] CompiledTemplate compileTemplate(Draft const& draft, std::regex const& regex)
    {
        if (!draft.urlTemplate)
            return { .urlTemplate = std::nullopt, .problem = std::nullopt };
        if (isBuiltinLinkPatternName(draft.name))
            return { .urlTemplate = std::nullopt, .problem = LinkPatternProblem::TemplateOnBuiltin };
        auto parsed = LinkUrlTemplate::parse(*draft.urlTemplate);
        if (!parsed)
            return { .urlTemplate = std::nullopt, .problem = LinkPatternProblem::InvalidUrlTemplate };
        if (parsed->highestGroup() > static_cast<int>(regex.mark_count()))
            return { .urlTemplate = std::nullopt, .problem = LinkPatternProblem::UnknownTemplateGroup };
        return { .urlTemplate = std::move(*parsed), .problem = std::nullopt };
    }

    [[nodiscard]] bool canMatchEmpty(std::regex const& regex)
    {
        return std::ranges::any_of(EmptyMatchProbes, [&](std::string_view probe) {
            auto const matches =
                std::ranges::subrange(std::cregex_iterator(probe.data(), probe.data() + probe.size(), regex),
                                      std::cregex_iterator());
            return std::ranges::any_of(matches,
                                       [](std::cmatch const& match) { return match.length(0) == 0; });
        });
    }

    /// Whether a `[…]` class in @p source holds a byte outside ASCII.
    [[nodiscard]] bool hasNonAsciiInBracket(std::string_view source)
    {
        return anyRegexSourceCharacter(source, [](char ch, RegexSourceRole role) {
            return role == RegexSourceRole::InClass && static_cast<unsigned char>(ch) >= 0x80;
        });
    }
} // namespace

LinkPatternSeverity LinkPatternDiagnostic::severity() const noexcept
{
    switch (problem)
    {
        case LinkPatternProblem::NonAsciiInBracket:
        case LinkPatternProblem::TemplateOnBuiltin:
        case LinkPatternProblem::PrintedScheme:
        case LinkPatternProblem::ClickableBuiltinWithoutScheme: return LinkPatternSeverity::Warning;
        case LinkPatternProblem::InvalidRegex:
        case LinkPatternProblem::MatchesEmpty:
        case LinkPatternProblem::UnknownBuiltin:
        case LinkPatternProblem::BackReference:
        case LinkPatternProblem::InvalidUrlTemplate:
        case LinkPatternProblem::UnknownTemplateGroup:
        case LinkPatternProblem::PathWithoutGroup: return LinkPatternSeverity::Error;
    }
    return LinkPatternSeverity::Error;
}

std::string LinkPatternDiagnostic::message() const
{
    auto const what = [this]() -> std::string {
        switch (problem)
        {
            case LinkPatternProblem::InvalidRegex:
                return std::format("invalid regex ({}); it is disabled", detail);
            case LinkPatternProblem::MatchesEmpty:
                return "a clickable pattern must not match the empty string; it is disabled";
            case LinkPatternProblem::NonAsciiInBracket:
                return "a non-ASCII character inside [...] matches byte by byte, not as one character";
            case LinkPatternProblem::UnknownBuiltin:
                return "names no built-in pattern and has no regex; it is ignored";
            case LinkPatternProblem::BackReference:
                return "back-references (\\1 to \\9) are not supported; it is disabled";
            case LinkPatternProblem::InvalidUrlTemplate:
                return std::format(
                    "url '{}' is no URL template, in which {{0}} is the match and {{1}} to {{9}} "
                    "its groups; it is disabled",
                    detail);
            case LinkPatternProblem::UnknownTemplateGroup:
                return std::format("url '{}' names a capture group the regex does not have; it is disabled",
                                   detail);
            case LinkPatternProblem::TemplateOnBuiltin:
                return "url applies to user patterns only, as a built-in opens what it finds; it is ignored";
            case LinkPatternProblem::PathWithoutGroup:
                return "a path pattern names the path with capture group 1 (its line and column with "
                       "groups 2 to 5), which it does not have; it is disabled";
            case LinkPatternProblem::PrintedScheme:
                return "its link takes its scheme from the printed text, so a match is a link "
                       "only where that names http, https, ftp, mailto or file; "
                       "write the scheme at the start of url";
            case LinkPatternProblem::ClickableBuiltinWithoutScheme:
                return "clickable has no effect on this built-in: "
                       "what it finds names no scheme a link opens; "
                       "to link its matches, write a pattern of your own whose url starts with one";
        }
        return {};
    }();
    return std::format("Link pattern '{}': {}.", pattern, what);
}

LinkPatterns LinkPatterns::compile(std::span<LinkPatternSpec const> entries)
{
    auto drafts = std::vector<Draft> {};
    for (auto const& builtin: BuiltinPatterns)
        drafts.push_back(Draft { .name = std::string { builtin.name },
                                 .kind = builtin.kind,
                                 .activation = builtin.activation,
                                 .source = std::string { builtin.regex } });

    auto table = LinkPatterns {};
    auto unknownNames = std::vector<LinkPatternDiagnostic> {};
    for (auto const& entry: entries)
    {
        auto const existing = std::ranges::find(drafts, entry.name, &Draft::name);
        if (existing != drafts.end())
        {
            if (entry.regex)
                existing->source = *entry.regex;
            if (entry.activation)
                existing->activation = *entry.activation;
            if (entry.urlTemplate)
                existing->urlTemplate = entry.urlTemplate;
            continue;
        }
        if (!entry.regex)
        {
            unknownNames.push_back(
                { .pattern = entry.name, .problem = LinkPatternProblem::UnknownBuiltin, .detail = {} });
            continue;
        }
        drafts.push_back(Draft { .name = entry.name,
                                 .kind = LinkKind::Custom,
                                 .activation = entry.activation.value_or(LinkActivation::HintOnly),
                                 .source = *entry.regex,
                                 .urlTemplate = entry.urlTemplate });
    }

    for (auto& draft: drafts)
    {
        // Refused before compiling, so that every platform refuses it: only libstdc++'s engine would throw.
        if (hasRegexBackReference(std::string_view(draft.source)))
        {
            table._diagnostics.push_back(
                { .pattern = draft.name, .problem = LinkPatternProblem::BackReference, .detail = {} });
            continue;
        }
        auto regex = std::regex {};
        try
        {
            regex = std::regex(draft.source, RegexEngineFlags);
        }
        catch (std::regex_error const& error)
        {
            table._diagnostics.push_back({ .pattern = draft.name,
                                           .problem = LinkPatternProblem::InvalidRegex,
                                           .detail = error.what() });
            continue;
        }
        if (hasNonAsciiInBracket(draft.source))
            table._diagnostics.push_back(
                { .pattern = draft.name, .problem = LinkPatternProblem::NonAsciiInBracket, .detail = {} });
        // A path pattern names the path with group 1 (LinkMatcher): without it, it would find nothing.
        if (draft.kind == LinkKind::Path && regex.mark_count() < 1)
        {
            table._diagnostics.push_back(
                { .pattern = draft.name, .problem = LinkPatternProblem::PathWithoutGroup, .detail = {} });
            continue;
        }
        auto compiled = compileTemplate(draft, regex);
        if (compiled.problem)
        {
            auto& diagnostic = table._diagnostics.emplace_back(LinkPatternDiagnostic {
                .pattern = draft.name, .problem = *compiled.problem, .detail = *draft.urlTemplate });
            if (diagnostic.severity() == LinkPatternSeverity::Error)
                continue;
        }
        if (draft.activation == LinkActivation::Clickable && canMatchEmpty(regex))
        {
            table._diagnostics.push_back(
                { .pattern = draft.name, .problem = LinkPatternProblem::MatchesEmpty, .detail = {} });
            continue;
        }
        // Decided here, once: the template knows whether it writes its own scheme (§7.6).
        if (draft.activation == LinkActivation::Clickable && draft.kind == LinkKind::Custom
            && (!compiled.urlTemplate || compiled.urlTemplate->schemeSource() == LinkSchemeSource::Printed))
            table._diagnostics.push_back({ .pattern = draft.name,
                                           .problem = isBuiltinLinkPatternName(draft.name)
                                                          ? LinkPatternProblem::ClickableBuiltinWithoutScheme
                                                          : LinkPatternProblem::PrintedScheme,
                                           .detail = {} });
        table._patterns.push_back(LinkPattern { .name = std::move(draft.name),
                                                .kind = draft.kind,
                                                .activation = draft.activation,
                                                .source = std::move(draft.source),
                                                .regex = std::move(regex),
                                                .urlTemplate = std::move(compiled.urlTemplate) });
    }
    std::ranges::move(unknownNames, std::back_inserter(table._diagnostics));
    return table;
}

std::shared_ptr<LinkPatterns const> LinkPatterns::builtins()
{
    static auto const table = std::make_shared<LinkPatterns const>(compile({}));
    return table;
}

LinkPattern const* LinkPatterns::find(std::string_view name) const noexcept
{
    auto const found = std::ranges::find(_patterns, name, &LinkPattern::name);
    return found != _patterns.end() ? &*found : nullptr;
}

bool LinkPatterns::hasClickable() const noexcept
{
    return std::ranges::any_of(_patterns, [](LinkPattern const& pattern) {
        return pattern.activation == LinkActivation::Clickable;
    });
}

std::span<std::string_view const> builtinLinkPatternNames() noexcept
{
    return BuiltinNames;
}

bool isBuiltinLinkPatternName(std::string_view name) noexcept
{
    return std::ranges::find(BuiltinNames, name) != BuiltinNames.end();
}

} // namespace vtbackend
