// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/Config.hpp>
#include <contour/input/KeyEvent.hpp>

#include <vtbackend/input/InputGenerator.hpp>

#include <cstdint>
#include <variant>

namespace contour::input
{

/// One binding that may claim an event: its actions, and what a press whose actions all decline does.
struct BindingCandidate
{
    config::ActionList const* actions = nullptr; ///< The bound actions; nullptr when nothing matched.
    config::DeclinedInput declined = config::DeclinedInput::PassThrough;
};

/// Send the event to the terminal as a named key.
struct SendKey
{
    vtbackend::Key key {};
    vtbackend::KeyboardModifiers modifiers {};
};

/// Send the event to the terminal as a character.
struct SendChar
{
    char32_t codepoint = 0;
    vtbackend::KeyIdentity identity {};
    vtbackend::KeyboardModifiers modifiers {};
};

/// Write nothing: the event is one the terminal cannot represent, and no binding wanted it.
struct ConsumeInput
{
};

/// What reaches the terminal once every binding candidate has declined.
using TerminalOutcome = std::variant<SendKey, SendChar, ConsumeInput>;

/// The plan for one key or character event: try the user's binding, then the built-in one, then do
/// `otherwise`.
///
/// A plan rather than a verdict, because whether a binding claims an event is only known once its actions
/// have run (one may decline, as CopySelection does with nothing selected).
struct KeyRoute
{
    BindingCandidate user {};     ///< The user's `input_mapping:` row. Its declined press always passes on.
    BindingCandidate fallback {}; ///< The built-in fallback row, with that row's own decline policy.
    TerminalOutcome otherwise {}; ///< What the terminal receives when neither claims the event.
};

/// Whether the terminal's active keyboard encoding can carry the Super modifier on a character.
enum class SuperEncoding : uint8_t
{
    Unrepresentable = 0, //!< Legacy encoding: a Super chord would arrive as the bare character.
    Representable,       //!< A non-legacy kitty mode: Super is a modifier bit like any other.
};

/// The state a route is decided in.
struct RouteContext
{
    config::Config const& config; ///< The bindings: the user's mappings and the built-in fallback rows.
    uint8_t modes;                ///< The binding modes that hold (vtbackend::MatchModes::Flag bits).
    SuperEncoding superEncoding;  ///< What the terminal can encode.
};

/// Routes a named key.
/// @param key The key.
/// @param modifiers The chord and locks; bindings match the chord only.
/// @param appModifier Whether the platform's application modifier is held.
/// @param context The bindings and the terminal state.
/// @return The plan; its `otherwise` is always SendKey, for a named key encodes every modifier.
[[nodiscard]] KeyRoute routeKey(vtbackend::Key key,
                                vtbackend::KeyboardModifiers modifiers,
                                AppModifier appModifier,
                                RouteContext const& context);

/// Routes a character.
///
/// Bindings are stored folded (see YAMLConfigReader::parseKeyOrChar) and matched folded; a Shift chord that
/// misses is retried under its US-ASCII unshifted base, so `Ctrl+Shift+,` fires on the '<' Qt delivers.
/// @param codepoint The character.
/// @param identity The key that produced it.
/// @param modifiers The chord and locks; bindings match the chord only.
/// @param appModifier Whether the platform's application modifier is held.
/// @param context The bindings and the terminal state.
/// @return The plan. An unbound character under the application modifier is consumed while the encoding
///         cannot carry Super -- it would otherwise arrive as the bare letter; any other is sent.
[[nodiscard]] KeyRoute routeChar(char32_t codepoint,
                                 vtbackend::KeyIdentity identity,
                                 vtbackend::KeyboardModifiers modifiers,
                                 AppModifier appModifier,
                                 RouteContext const& context);

} // namespace contour::input
