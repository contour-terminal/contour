// SPDX-License-Identifier: Apache-2.0
#pragma once

#ifdef __linux__

    #include <QtCore/QLatin1StringView>
    #include <QtCore/QObject>
    #include <QtCore/QVariantList>

    #include <cstddef>
    #include <cstdint>
    #include <functional>
    #include <string_view>

namespace contour::platform
{

/// The one D-Bus name every xdg-desktop-portal interface is reached at.
///
/// A sandbox has exactly one entry point to the desktop, so there is nothing to discover: the
/// service and path are fixed and only the interface varies between portals.
constexpr auto PortalService = QLatin1StringView("org.freedesktop.portal.Desktop");

/// The object path every xdg-desktop-portal interface is reached at. @see PortalService.
constexpr auto PortalPath = QLatin1StringView("/org/freedesktop/portal/desktop");

/// A QLatin1StringView as a std::string_view, for formatting.
///
/// Explicit rather than a braced conversion: QLatin1StringView::size() is qsizetype (signed), and
/// narrowing it inside braces is a -Wc++11-narrowing error under this tree's -Werror.
///
/// @param text The Qt view; its storage must outlive the result, as a string literal's does.
/// @return The same characters, as the standard view.
[[nodiscard]] constexpr std::string_view asStringView(QLatin1StringView text) noexcept
{
    return std::string_view { text.data(), static_cast<size_t>(text.size()) };
}

/// Whether a portal method call was accepted.
///
/// An enum rather than a bool because at the call site `true` says nothing about which way round the
/// question was asked, and the two states have real names. The reason for a failure is not carried:
/// nothing acts on it beyond the log line this already writes.
enum class CallOutcome : uint8_t
{
    Failed = 0,
    Accepted = 1,
};

/// Issues one xdg-desktop-portal method call, returning WITHOUT waiting for it.
///
/// Injected because the D-Bus call is the one ambient resource the portal-speaking classes are built
/// out of. Behind a seam, a headless test drives a whole transport or launcher -- its bookkeeping,
/// its fallbacks, what it does when a call is ACCEPTED -- with no portal to send to; without one,
/// reaching that code would mean raising real notifications at, or opening real browsers on,
/// whoever runs the suite.
///
/// @param context Whose lifetime the pending call is bound to.
/// @param method The interface method to call.
/// @param arguments Its arguments, in wire order.
/// @param onReply Called if and when a reply arrives, with whether the call was accepted. Empty
///                where the caller does not read the reply at all.
using PortalCaller = std::function<void(QObject* context,
                                        QLatin1StringView method,
                                        QVariantList const& arguments,
                                        std::function<void(CallOutcome)> onReply)>;

/// The production PortalCaller for @p interface: an asynchronous session-bus call to that portal,
/// whose reply watcher is owned by the context object.
///
/// @param interface The portal interface to address, e.g. org.freedesktop.portal.OpenURI. It must
///                  name storage that outlives the returned caller -- a string literal, as every
///                  caller passes.
/// @return The caller.
[[nodiscard]] PortalCaller qtPortalCaller(QLatin1StringView interface);

} // namespace contour::platform

#endif // defined(__linux__)
