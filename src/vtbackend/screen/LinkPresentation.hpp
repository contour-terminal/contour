// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/LinkDecoration.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace vtbackend
{

/// Where the pointer is, as far as one link is concerned.
enum class LinkPointer : uint8_t
{
    Elsewhere = 0, ///< Not over this link, or over it while a click would go to the application.
    Over,          ///< Over this link, and a click would follow it.
};

/// How many LinkPointer values there are, derived from the last one.
inline constexpr size_t LinkPointerCount = static_cast<size_t>(LinkPointer::Over) + 1;

/// Whether the follow gesture's modifiers (Ctrl, by default) are held.
enum class FollowModifiers : uint8_t
{
    Released = 0,
    Held,
};

/// How many FollowModifiers values there are, derived from the last one.
inline constexpr size_t FollowModifiersCount = static_cast<size_t>(FollowModifiers::Held) + 1;

/// The pointer shape a link asks for.
enum class LinkPointerShape : uint8_t
{
    Default = 0, ///< Whatever the terminal shows otherwise.
    Hand,        ///< The pointing hand: a click here follows a link.
};

/// Whether a held or released modifier changed how links are presented.
enum class LinkRevealChange : uint8_t
{
    Unchanged = 0,
    Changed,
};

/// How one link is presented this frame.
struct LinkPresentation
{
    LinkDecorationRole role = LinkDecorationRole::None;   ///< How the link's cells are decorated.
    LinkPointerShape pointer = LinkPointerShape::Default; ///< The pointer shape the link asks for.

    bool operator==(LinkPresentation const&) const = default;
};

/// One row of the presentation table.
struct LinkPresentationRow
{
    LinkDecorationMode mode = LinkDecorationMode::Always;  ///< hyperlink_decoration.mode.
    LinkPointer pointer = LinkPointer::Elsewhere;          ///< Where the pointer is.
    FollowModifiers modifiers = FollowModifiers::Released; ///< Whether the follow modifiers are held.
    LinkPresentation presentation;                         ///< What the three above lead to.
};

// clang-format off
/// §6.1 of the link-detection design. The mouse-owner rule is the caller's: while an application takes clicks
/// (mouse tracking on, the bypass modifier not held) the pointer counts as Elsewhere.
inline constexpr auto LinkPresentationTable = std::array {
    LinkPresentationRow { LinkDecorationMode::Always,   LinkPointer::Elsewhere, FollowModifiers::Released, { LinkDecorationRole::Normal, LinkPointerShape::Default } },
    LinkPresentationRow { LinkDecorationMode::Always,   LinkPointer::Elsewhere, FollowModifiers::Held,     { LinkDecorationRole::Normal, LinkPointerShape::Default } },
    LinkPresentationRow { LinkDecorationMode::Always,   LinkPointer::Over,      FollowModifiers::Released, { LinkDecorationRole::Hover,  LinkPointerShape::Hand    } },
    LinkPresentationRow { LinkDecorationMode::Always,   LinkPointer::Over,      FollowModifiers::Held,     { LinkDecorationRole::Hover,  LinkPointerShape::Hand    } },
    LinkPresentationRow { LinkDecorationMode::Hover,    LinkPointer::Elsewhere, FollowModifiers::Released, { LinkDecorationRole::None,   LinkPointerShape::Default } },
    LinkPresentationRow { LinkDecorationMode::Hover,    LinkPointer::Elsewhere, FollowModifiers::Held,     { LinkDecorationRole::None,   LinkPointerShape::Default } },
    LinkPresentationRow { LinkDecorationMode::Hover,    LinkPointer::Over,      FollowModifiers::Released, { LinkDecorationRole::Hover,  LinkPointerShape::Hand    } },
    LinkPresentationRow { LinkDecorationMode::Hover,    LinkPointer::Over,      FollowModifiers::Held,     { LinkDecorationRole::Hover,  LinkPointerShape::Hand    } },
    LinkPresentationRow { LinkDecorationMode::Modifier, LinkPointer::Elsewhere, FollowModifiers::Released, { LinkDecorationRole::None,   LinkPointerShape::Default } },
    LinkPresentationRow { LinkDecorationMode::Modifier, LinkPointer::Elsewhere, FollowModifiers::Held,     { LinkDecorationRole::Normal, LinkPointerShape::Default } },
    LinkPresentationRow { LinkDecorationMode::Modifier, LinkPointer::Over,      FollowModifiers::Released, { LinkDecorationRole::None,   LinkPointerShape::Default } },
    LinkPresentationRow { LinkDecorationMode::Modifier, LinkPointer::Over,      FollowModifiers::Held,     { LinkDecorationRole::Hover,  LinkPointerShape::Hand    } },
    LinkPresentationRow { LinkDecorationMode::Never,    LinkPointer::Elsewhere, FollowModifiers::Released, { LinkDecorationRole::None,   LinkPointerShape::Default } },
    LinkPresentationRow { LinkDecorationMode::Never,    LinkPointer::Elsewhere, FollowModifiers::Held,     { LinkDecorationRole::None,   LinkPointerShape::Default } },
    LinkPresentationRow { LinkDecorationMode::Never,    LinkPointer::Over,      FollowModifiers::Released, { LinkDecorationRole::None,   LinkPointerShape::Default } },
    LinkPresentationRow { LinkDecorationMode::Never,    LinkPointer::Over,      FollowModifiers::Held,     { LinkDecorationRole::None,   LinkPointerShape::Hand    } },
};
// clang-format on

/// How a link is presented.
/// @param mode hyperlink_decoration.mode.
/// @param pointer Where the pointer is, the mouse-owner rule already applied.
/// @param modifiers Whether the follow gesture's modifiers are held.
/// @return The decoration role and the pointer shape.
[[nodiscard]] constexpr LinkPresentation presentLink(LinkDecorationMode mode,
                                                     LinkPointer pointer,
                                                     FollowModifiers modifiers) noexcept
{
    for (auto const& row: LinkPresentationTable)
        if (row.mode == mode && row.pointer == pointer && row.modifiers == modifiers)
            return row.presentation;
    return {};
}

} // namespace vtbackend
