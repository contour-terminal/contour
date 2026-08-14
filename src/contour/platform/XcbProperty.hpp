// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QtGui/QWindow>

// Where xcb exists: not Windows, not macOS -- so UNIX in general, the BSDs as well as Linux.
//
// Defined HERE rather than in each translation unit that needs it, so the callers cannot disagree
// about it. Everything below is declared only inside the guard, which means a caller that forgets
// the guard fails to compile rather than silently linking against nothing.
#if !defined(Q_OS_WINDOWS) && !defined(Q_OS_DARWIN)
    #define CONTOUR_FRONTEND_XCB
#endif

#ifdef CONTOUR_FRONTEND_XCB

    #include <cstdint>
    #include <cstdlib>
    #include <memory>
    #include <optional>
    #include <span>
    #include <string>

    #include <xcb/xproto.h>

namespace contour::platform
{

/// Owns a libxcb reply, which the caller must free.
///
/// Every xcb_*_reply() hands back a malloc'd block; forgetting one leaks silently. Declared here
/// rather than privately in the .cpp so the adapters that issue their own requests state the rule
/// the same way instead of hand-writing free() with a clang-tidy suppression.
struct XcbReplyDeleter
{
    void operator()(void* reply) const noexcept
    {
        std::free(reply);
    }
};

template <typename T>
using XcbReply = std::unique_ptr<T, XcbReplyDeleter>;

/// A window's xcb identity paired with one interned property atom.
///
/// The three things every property operation needs, resolved once. X11WindowShadow holds one of
/// these for its window's lifetime rather than re-interning the atom on every shadow update.
struct XcbPropertyInfo
{
    xcb_connection_t* connection;
    xcb_window_t window;
    xcb_atom_t propertyAtom;
};

/// Resolves @p window's xcb identity and interns the property atom named @p name.
///
/// @param window The window whose id is wanted. Note this calls QWindow::winId(), which CREATES
///               the platform window if it does not exist yet -- do not call it on a window that
///               is deliberately still unmapped.
/// @param name   The X11 property name to intern, e.g. "_KDE_NET_WM_SHADOW".
/// @return nullopt when there is no X11 connection, i.e. when not running on the xcb platform.
[[nodiscard]] std::optional<XcbPropertyInfo> queryXcbPropertyInfo(QWindow* window, std::string const& name);

/// Sets a single-element CARDINAL property, replacing any previous value.
void setPropertyX11(QWindow* window, std::string const& name, uint32_t value);

/// Sets a STRING property, replacing any previous value.
void setPropertyX11(QWindow* window, std::string const& name, std::string const& value);

/// Sets a CARDINAL[32] array property, replacing any previous value.
///
/// `_KDE_NET_WM_SHADOW`'s twelve-element form is the only caller today; it sits beside the two
/// scalar overloads it was added next to rather than in the shadow adapter, because "write an
/// array of cardinals to a window property" is not a shadow concept.
void setPropertyX11(QWindow* window, std::string const& name, std::span<uint32_t const> values);

/// Deletes a property, if it is set.
void unsetPropertyX11(QWindow* window, std::string const& name);

} // namespace contour::platform

#endif
