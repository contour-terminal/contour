// SPDX-License-Identifier: Apache-2.0
#include <contour/input/KeyMapping.hpp>
#include <contour/input/KeyRouter.hpp>

namespace contour::input
{

namespace
{
    /// @return The candidate a built-in row makes: its actions, with the row's own decline policy.
    template <typename Input>
    [[nodiscard]] BindingCandidate candidateOf(config::FallbackMapping<Input> const* row) noexcept
    {
        if (row == nullptr)
            return {};
        return BindingCandidate { .actions = &row->mapping.binding, .declined = row->declined };
    }

    /// Looks @p codepoint up through @p find folded, and -- when that misses under Shift -- under its
    /// unshifted base. The two normalizations are disjoint (unshiftedCodepoint() rewrites only digits and
    /// punctuation, foldedBindingCodepoint() only ASCII letters), so they compose in either order.
    template <typename Find>
    [[nodiscard]] auto lookUpFolded(char32_t codepoint, vtbackend::Modifiers chord, Find const& find)
    {
        auto const folded = config::foldedBindingCodepoint(codepoint);
        auto const* found = find(folded);
        if (found == nullptr && chord.test(vtbackend::Modifier::Shift))
            if (auto const base = unshiftedCodepoint(folded); base != folded)
                found = find(base);
        return found;
    }
} // namespace

KeyRoute routeKey(vtbackend::Key key,
                  vtbackend::KeyboardModifiers modifiers,
                  AppModifier /*appModifier*/,
                  RouteContext const& context)
{
    // Key bindings match on the chord: a latched lock key must not change which shortcut fires.
    auto const* user =
        config::apply(context.config.inputMappings.value().keyMappings, key, modifiers.chord, context.modes);

    // The built-in rows second, on purpose: an explicit binding in the user's config always wins, because it
    // is found first (see builtinFallbackKeyMappings for why a plain default could not reach an existing user
    // at all).
    auto const* row = config::builtinFallbackRow(context.config, key, modifiers.chord, context.modes);

    return KeyRoute {
        .user = BindingCandidate { .actions = user, .declined = config::DeclinedInput::PassThrough },
        .fallback = candidateOf(row),
        .otherwise = SendKey { .key = key, .modifiers = modifiers },
    };
}

KeyRoute routeChar(char32_t codepoint,
                   vtbackend::KeyIdentity identity,
                   vtbackend::KeyboardModifiers modifiers,
                   AppModifier appModifier,
                   RouteContext const& context)
{
    // Folded, because WHICH case a letter arrives in is decided by the route the platform took rather than
    // by the user -- and the Option-as-Alt rule even lets CapsLock decide. Only the lookup is folded: the
    // codepoint itself is sent verbatim, so what the application receives is unchanged.
    auto const& charMappings = context.config.inputMappings.value().charMappings;
    auto const* user = lookUpFolded(codepoint, modifiers.chord, [&](char32_t ch) {
        return config::apply(charMappings, ch, modifiers.chord, context.modes);
    });
    auto const* row = lookUpFolded(codepoint, modifiers.chord, [&](char32_t ch) {
        return config::builtinFallbackRow(context.config, ch, modifiers.chord, context.modes);
    });

    // Never typed as a bare letter: a legacy encoding drops Super, so an unbound application-modifier chord
    // would reach the program as the character alone. A named key needs no such rule -- its modifier
    // parameter carries Super -- and a non-legacy kitty mode encodes Super as the protocol specifies.
    auto const unrepresentable =
        appModifier == AppModifier::Held && context.superEncoding == SuperEncoding::Unrepresentable;
    auto otherwise = unrepresentable
                         ? TerminalOutcome { ConsumeInput {} }
                         : TerminalOutcome { SendChar {
                               .codepoint = codepoint, .identity = identity, .modifiers = modifiers } };

    return KeyRoute {
        .user = BindingCandidate { .actions = user, .declined = config::DeclinedInput::PassThrough },
        .fallback = candidateOf(row),
        .otherwise = otherwise,
    };
}

} // namespace contour::input
