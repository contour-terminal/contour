// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/AgentHookConfig.hpp>
#include <contour/cli/AgentIntegrations.hpp>

#include <core/Utils.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <format>
#include <optional>
#include <ranges>
#include <vector>

using namespace std::string_view_literals;

namespace contour::cli
{

namespace
{
    using Object = glz::generic::object_t;
    using Array = glz::generic::array_t;

    /// @param integration An agent.
    /// @return What its hook commands carry after the binary, and what recognizes them as ours.
    [[nodiscard]] std::string markerOf(AgentIntegration const& integration)
    {
        return std::format("agent report --agent {}", integration.integrationId);
    }

    constexpr auto HookTimeoutSeconds = 5.0;

    /// The characters shellWord() leaves a binary's path unquoted with, beside letters and digits.
    constexpr auto PlainShellCharacters = "/._-+:@%"sv;

    /// Whether an edit of a document changed it.
    enum class Edit : uint8_t
    {
        None = 0, ///< Nothing changed.
        Made,     ///< Something did.
    };

    /// @return Made when either of @p a and @p b is.
    [[nodiscard]] constexpr Edit operator|(Edit a, Edit b) noexcept
    {
        return a == Edit::Made || b == Edit::Made ? Edit::Made : Edit::None;
    }

    /// The glaze options a settings file is written with: pretty, two spaces per level.
    struct PrettyTwoSpaces: glz::opts
    {
        uint8_t indentation_width = 2; // NOLINT(readability-identifier-naming)
    };

    /// glz::generic's braced construction from a map or array builds an array holding it, so values are made
    /// by assigning the variant.
    /// @param value A string, number, array or object.
    /// @return The value as a document node.
    template <typename T>
    [[nodiscard]] glz::generic nodeOf(T value)
    {
        auto node = glz::generic {};
        node.data = std::move(value);
        return node;
    }

    /// @param text Any text.
    /// @return Whether it holds only whitespace.
    [[nodiscard]] bool isBlank(std::string_view text) noexcept
    {
        return std::ranges::all_of(text,
                                   [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; });
    }

    /// @return Whether @p word is one a shell neither splits nor expands, so it needs no quotes.
    [[nodiscard]] bool isPlainShellWord(std::string_view word) noexcept
    {
        return !word.empty() && std::ranges::all_of(word, [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) != 0 || PlainShellCharacters.contains(c);
        });
    }

    /// @param exe A binary path.
    /// @return It as one shell word: quoted when it holds anything a shell would split or expand.
    [[nodiscard]] std::string shellWord(std::string_view exe)
    {
        if (isPlainShellWord(exe))
            return std::string { exe };
        auto word = std::string { "'" };
        for (auto const c: exe)
            word += c == '\'' ? "'\\''"sv : std::string_view { &c, 1 };
        word += '\'';
        return word;
    }

    /// @param document A document.
    /// @return It as JSON, two-space indented.
    [[nodiscard]] std::string render(glz::generic const& document)
    {
        auto text = std::string {};
        (void) glz::write<PrettyTwoSpaces { { .prettify = true } }>(document, text);
        text += '\n';
        return text;
    }

    [[nodiscard]] HookMerge malformed(std::string detail)
    {
        return HookMerge { .result = HookMergeResult::Malformed, .text = {}, .detail = std::move(detail) };
    }

    /// @return What an edit of @p settingsJson came to: the text as it was when @p edit made none, else
    ///         @p document rendered.
    [[nodiscard]] HookMerge mergeOf(Edit edit, std::string_view settingsJson, glz::generic const& document)
    {
        if (edit == Edit::None)
            return HookMerge { .result = HookMergeResult::Unchanged,
                               .text = std::string { settingsJson },
                               .detail = {} };
        return HookMerge { .result = HookMergeResult::Changed, .text = render(document), .detail = {} };
    }

    /// Reads a settings document: an object, or nothing for a blank text.
    [[nodiscard]] std::optional<glz::generic> readSettings(std::string_view json)
    {
        auto document = nodeOf(Object {});
        if (isBlank(json))
            return document;
        if (glz::read_json(document, json) || document.get_if<Object>() == nullptr)
            return std::nullopt;
        return document;
    }

    /// @param word A word shellWord() may have made.
    /// @return The text it quotes: the word itself when it is plain, the text within its single quotes
    ///         (each `'\''` an apostrophe) when it is quoted; nothing when shellWord() makes no such word.
    [[nodiscard]] std::optional<std::string> unquotedShellWord(std::string_view word)
    {
        if (isPlainShellWord(word))
            return std::string { word };
        constexpr auto Apostrophe = "'\\''"sv;
        if (word.size() < 2 || !word.starts_with('\'') || !word.ends_with('\''))
            return std::nullopt;
        auto inner = word.substr(1, word.size() - 2);
        auto text = std::string {};
        while (!inner.empty())
        {
            if (inner.starts_with(Apostrophe))
            {
                text += '\'';
                inner.remove_prefix(Apostrophe.size());
            }
            else if (inner.front() == '\'')
                return std::nullopt; // a quote that ends the word early: more than one word
            else
            {
                text += inner.front();
                inner.remove_prefix(1);
            }
        }
        return text;
    }

    /// @param command A hook's command line.
    /// @param marker The text of markerOf() that recognizes our commands.
    /// @return Whether it is exactly the command ourHook() writes -- one binary word, then @p marker -- and
    ///         not a user's own command that merely mentions it (a wrapper, a pipeline, extra arguments).
    [[nodiscard]] bool isOurCommand(std::string_view command, std::string_view marker)
    {
        if (!command.ends_with(marker) || command.size() < marker.size() + 2)
            return false;
        auto const binary = command.substr(0, command.size() - marker.size());
        if (!binary.ends_with(' '))
            return false;
        auto const word = binary.substr(0, binary.size() - 1);
        return unquotedShellWord(word)
            .transform([](std::string const& exe) { return !exe.empty(); })
            .value_or(false);
    }

    /// @param hook A hook entry (`{"type":"command","command":...}`).
    /// @param marker The text of markerOf() that recognizes our commands.
    /// @return Whether it is ours.
    [[nodiscard]] bool isOurs(glz::generic const& hook, std::string_view marker)
    {
        auto const* object = hook.get_if<Object>();
        if (object == nullptr)
            return false;
        auto const command = object->find("command");
        if (command == object->end())
            return false;
        auto const* text = command->second.get_if<std::string>();
        return text != nullptr && isOurCommand(*text, marker);
    }

    [[nodiscard]] glz::generic ourHook(std::string_view contourExe, std::string_view marker)
    {
        return nodeOf(Object { { "type", nodeOf(std::string { "command" }) },
                               { "command", nodeOf(std::format("{} {}", shellWord(contourExe), marker)) },
                               { "timeout", nodeOf(HookTimeoutSeconds) } });
    }

    /// @param node A document node.
    /// @return Its compact JSON, for comparing nodes.
    [[nodiscard]] std::string compact(glz::generic const& node)
    {
        auto text = std::string {};
        (void) glz::write_json(node, text);
        return text;
    }

    /// @param group A matcher group.
    /// @return Its `hooks` array, or null when it has none.
    [[nodiscard]] Array* hooksOf(glz::generic& group)
    {
        auto* object = group.get_if<Object>();
        if (object == nullptr)
            return nullptr;
        auto const hooks = object->find("hooks");
        return hooks != object->end() ? hooks->second.get_if<Array>() : nullptr;
    }

    /// Makes @p groups hold our hook once, as @p wanted. @return Whether that changed them.
    [[nodiscard]] Edit ensureOurHook(Array& groups, glz::generic const& wanted, std::string_view marker)
    {
        auto const wantedText = compact(wanted);
        for (auto& group: groups)
            if (auto* hooks = hooksOf(group))
                for (auto& hook: *hooks)
                    if (isOurs(hook, marker))
                    {
                        if (compact(hook) == wantedText)
                            return Edit::None;
                        hook = wanted;
                        return Edit::Made;
                    }
        groups.emplace_back(
            nodeOf(Object { { "matcher", nodeOf(std::string {}) }, { "hooks", nodeOf(Array { wanted }) } }));
        return Edit::Made;
    }

    /// Removes our hooks from @p groups, and each group that held nothing else -- not a group the user left
    /// empty. @return Whether that changed them.
    [[nodiscard]] Edit removeOurHooks(Array& groups, std::string_view marker)
    {
        auto edit = Edit::None;
        auto emptied = std::vector<std::size_t> {};
        for (auto&& [index, group]: core::views::enumerate(groups))
            if (auto* hooks = hooksOf(group))
                if (std::erase_if(*hooks, [&](glz::generic const& hook) { return isOurs(hook, marker); }) > 0)
                {
                    edit = Edit::Made;
                    if (hooks->empty())
                        emptied.push_back(static_cast<std::size_t>(index));
                }
        for (auto const index: emptied | std::views::reverse)
            groups.erase(groups.begin() + static_cast<std::ptrdiff_t>(index));
        return edit;
    }

    /// @return The document's `hooks` object, created when absent; null when it is something else.
    [[nodiscard]] Object* ensuredHooksObject(glz::generic& document)
    {
        auto& root = *document.get_if<Object>();
        auto found = root.find("hooks");
        if (found == root.end())
            found = root.emplace("hooks", nodeOf(Object {})).first;
        return found->second.get_if<Object>();
    }

    /// @return The document's `hooks` object; null when it is absent or something else.
    [[nodiscard]] Object* existingHooksObject(glz::generic& document)
    {
        auto& root = *document.get_if<Object>();
        auto const found = root.find("hooks");
        return found != root.end() ? found->second.get_if<Object>() : nullptr;
    }
} // namespace

HookMerge mergeHooks(AgentIntegration const& integration,
                     std::string_view settingsJson,
                     std::string_view contourExe)
{
    auto document = readSettings(settingsJson);
    if (!document)
        return malformed("hook file is not a JSON object");
    auto* hooks = ensuredHooksObject(*document);
    if (hooks == nullptr)
        return malformed("\"hooks\" is not an object");

    auto const marker = markerOf(integration);
    auto const wanted = ourHook(contourExe, marker);
    auto edit = Edit::None;
    for (auto const event: integration.hookEvents)
    {
        auto& groups = (*hooks)[std::string { event }];
        if (groups.get_if<Array>() == nullptr)
        {
            if (!groups.holds<std::nullptr_t>())
                return malformed(std::format("\"hooks.{}\" is not an array", event));
            groups = nodeOf(Array {});
        }
        edit = edit | ensureOurHook(*groups.get_if<Array>(), wanted, marker);
    }
    return mergeOf(edit, settingsJson, *document);
}

HookMerge removeHooks(AgentIntegration const& integration, std::string_view settingsJson)
{
    auto document = readSettings(settingsJson);
    if (!document)
        return malformed("hook file is not a JSON object");
    auto* hooks = existingHooksObject(*document);
    auto const marker = markerOf(integration);
    auto edit = Edit::None;
    if (hooks != nullptr)
    {
        // Only what our removal emptied goes: an event array, or the `hooks` object, the user left empty
        // stays as it was.
        auto emptied = std::vector<std::string> {};
        for (auto& [event, groups]: *hooks)
            if (auto* array = groups.get_if<Array>();
                array != nullptr && removeOurHooks(*array, marker) == Edit::Made)
            {
                edit = Edit::Made;
                if (array->empty())
                    emptied.emplace_back(event);
            }
        for (auto const& event: emptied)
            hooks->erase(event);
        if (!emptied.empty() && hooks->empty())
            document->get_if<Object>()->erase("hooks");
    }
    return mergeOf(edit, settingsJson, *document);
}

} // namespace contour::cli
