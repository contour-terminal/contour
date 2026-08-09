// SPDX-License-Identifier: Apache-2.0
#include <contour/config/WindowControlStyle.hpp>

#include <crispy/ASCII.hpp>
#include <crispy/Environment.hpp>

#include <algorithm>
#include <string_view>

namespace contour::config
{

namespace
{
    /// Whether @p haystack contains @p needle, ignoring ASCII case.
    ///
    /// XDG_CURRENT_DESKTOP is a colon-separated list whose entries are conventionally upper case
    /// ("KDE", "ubuntu:GNOME") but not required to be, so this matches the way a desktop's own
    /// detection does: a case-insensitive substring, not an equality test against one spelling.
    ///
    /// The substring sibling of ModifierNames.hpp's sameSpelling, and folded the same way -- through
    /// crispy::ascii::fold, so the tree keeps one spelling of the ASCII case fold rather than one per
    /// caller.
    [[nodiscard]] constexpr bool containsIgnoreCase(std::string_view haystack,
                                                    std::string_view needle) noexcept
    {
        auto const sameLetter = [](char lhs, char rhs) noexcept {
            return crispy::ascii::fold(lhs) == crispy::ascii::fold(rhs);
        };
        return std::ranges::search(haystack, needle, sameLetter).begin() != haystack.end();
    }
} // namespace

HostPlatform detectDesktopPlatform(crispy::Environment const& env)
{
    // XDG_CURRENT_DESKTOP is the standardized answer, so where it says anything at all it is the
    // whole answer -- a desktop that named itself is not overruled by another desktop's marker.
    if (auto const desktop = env.get("XDG_CURRENT_DESKTOP"); desktop && !desktop->empty())
        return containsIgnoreCase(*desktop, "KDE") ? HostPlatform::KdePlasma : HostPlatform::Other;

    // Only once it said nothing: KDE's older marker, which covers the sessions -- and the su/sudo
    // shells -- where the XDG variable is not propagated. It is consulted last rather than as a
    // second chance because it LEAKS: a terminal, a `su -` or a nested session started from Plasma
    // carries KDE_FULL_SESSION into whatever runs there, so treating it as evidence against an
    // explicit `XDG_CURRENT_DESKTOP=ubuntu:GNOME` would draw Breeze's chrome on a GNOME desktop.
    if (auto const kde = env.get("KDE_FULL_SESSION"); kde && !kde->empty())
        return HostPlatform::KdePlasma;

    return HostPlatform::Other;
}

HostPlatform detectHostPlatform([[maybe_unused]] crispy::Environment const& env)
{
#ifdef _WIN32
    return HostPlatform::Windows;
#elifdef __APPLE__
    return HostPlatform::MacOS;
#else
    return detectDesktopPlatform(env);
#endif
}

} // namespace contour::config
