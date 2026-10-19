// SPDX-License-Identifier: Apache-2.0
#include <contour/command/Command.hpp>
#include <contour/command/Shortcut.hpp>

#include <libunicode/convert.h>

#include <algorithm>
#include <format>
#include <string>

namespace contour::command
{

namespace
{
    /// Renders the key or character a chord ends on: "Enter", "F3", "LeftArrow", "P".
    ///
    /// Reuses std::formatter<vtbackend::Key> rather than introducing prettier synonyms ("Left" for
    /// LeftArrow), for two reasons: a second key-name table would be one more thing to keep in step
    /// with the first, and the config's own vocabulary is the more useful one to show — a user who
    /// wants to REBIND what the palette just taught them types exactly this name into `input_mapping:`.
    [[nodiscard]] std::string inputText(ShortcutInput input)
    {
        return std::visit(core::Overloaded {
                              [](vtbackend::Key key) { return std::format("{}", key); },
                              [](char32_t ch) { return unicode::convert_to<char>(ch); },
                          },
                          input);
    }

    /// Indexes one half of the input mappings (the key half or the char half) into @p target.
    ///
    /// Templated over the binding's input type so the two halves share this logic rather than
    /// restating it — the only thing that differs between them is which member of ShortcutInput the
    /// binding's `input` lands in, and the variant absorbs that.
    template <typename Input>
    void indexBinding(vtbackend::InputBinding<Input, config::ActionList> const& binding,
                      std::unordered_map<std::string, std::string>& target)
    {
        // A chord that fires several actions is not "the" shortcut for any one of them: pressing it
        // would do all of the others too, so advertising it next to a single command would be a lie.
        if (binding.binding.size() != 1)
            return;

        // First binding wins, so a command bound twice renders under its first (config-order)
        // shortcut rather than flip-flopping between them.
        target.try_emplace(commandId(binding.binding.front()),
                           shortcutText(binding.modifiers, ShortcutInput { binding.input }));
    }

    template <typename Input>
    void indexBindings(std::vector<vtbackend::InputBinding<Input, config::ActionList>> const& bindings,
                       std::unordered_map<std::string, std::string>& target)
    {
        for (auto const& binding: bindings)
            indexBinding(binding, target);
    }

    /// Indexes the enabled built-in rows of one table whose chord @p userBindings leaves unbound.
    template <typename Input>
    void indexFallbacks(std::vector<config::FallbackMapping<Input>> const& rows,
                        std::vector<vtbackend::InputBinding<Input, config::ActionList>> const& userBindings,
                        config::Config const& config,
                        std::unordered_map<std::string, std::string>& target)
    {
        for (auto const& row: rows)
        {
            auto const shadowed = std::ranges::any_of(userBindings, [&row](auto const& binding) {
                return binding.modifiers == row.mapping.modifiers && binding.input == row.mapping.input;
            });
            if (row.enabled(config) && !shadowed)
                indexBinding(row.mapping, target);
        }
    }
} // namespace

std::string modifiersText(vtbackend::Modifiers modifiers)
{
    auto result = std::string {};

    for (auto const& row: ShortcutModifierTable)
    {
        if (!modifiers.test(row.modifier))
            continue;
        result += row.name;
        result += '+';
    }
    return result;
}

std::string shortcutText(vtbackend::Modifiers modifiers, ShortcutInput input)
{
    return modifiersText(modifiers) + inputText(input);
}

std::unordered_map<std::string, std::string> shortcutIndex(config::InputMappings const& mappings)
{
    auto result = std::unordered_map<std::string, std::string> {};

    // Mouse bindings are deliberately not indexed: the palette's shortcut column teaches the user what
    // to TYPE, and "Ctrl+Left-click" is not that.
    indexBindings(mappings.keyMappings, result);
    indexBindings(mappings.charMappings, result);

    return result;
}

std::unordered_map<std::string, std::string> shortcutIndex(config::Config const& config)
{
    auto const& mappings = config.inputMappings.value();
    auto result = shortcutIndex(mappings);
    indexFallbacks(config::builtinFallbackKeyMappings(), mappings.keyMappings, config, result);
    indexFallbacks(config::builtinFallbackCharMappings(), mappings.charMappings, config, result);
    return result;
}

std::string describeLinkGesture(config::Config const& config)
{
    auto const follow = config::followHyperlinkModifiers(config);
    if (!follow)
        return "No mouse binding follows links (input_mapping has no FollowHyperlink)";
    auto plain = modifiersText(*follow) + "Click";
    auto const bypass = config.bypassMouseProtocolModifiers.value();
    if (bypass == vtbackend::Modifiers {})
        return plain;
    return std::format(
        "{} ({}{} inside applications that use the mouse)", plain, modifiersText(bypass), plain);
}

} // namespace contour::command
