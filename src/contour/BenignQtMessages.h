// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <string_view>

namespace contour
{

/// One known-benign Qt-internal log message that Contour deliberately drops.
///
/// Some messages are emitted deep inside Qt for conditions that are neither actionable by us nor
/// fixable at their source; printing them only trains users to ignore Contour's real diagnostics.
/// Each entry documents *why* it is benign so the table stays self-explaining.
struct BenignQtMessage
{
    std::string_view substring {}; //!< Matched anywhere within the formatted message.
    std::string_view reason {};    //!< Why the message is benign / unfixable at its source (documentation).
};

/// The table of Qt-internal messages Contour suppresses. Data-driven on purpose: silencing a new
/// benign Qt message is adding one row here, not another guard scattered across the code.
inline constexpr auto BenignQtMessages = std::array {
    // QQuickDragAttachedPrivate::loadPixmap() unconditionally passes a sourceSize to QQuickPixmap::load
    // for the Drag.imageSource url. Our tab-drag ghost (TabItem.qml) is a grabToImage() result, whose
    // image provider rejects any sourceSize and warns "Ignoring sourceSize request for image url that
    // came from grabToImage." No QML property stops Qt requesting one (setting Drag.imageSourceSize only
    // makes it re-request on every change), so the custom ghost cannot avoid it — and the warning is
    // purely cosmetic: the ghost still renders.
    BenignQtMessage { std::string_view("came from grabToImage"),
                      std::string_view("Qt always requests a sourceSize on Drag.imageSource grab urls") },
};

/// Decides whether a formatted Qt log message is a known-benign one Contour should not print.
///
/// Pure and dependency-free (operates on the already-formatted text) so it is unit-testable without a
/// Qt message handler or a running QGuiApplication.
///
/// @param message The fully formatted Qt log message text.
/// @return true iff @p message contains the substring of any @ref BenignQtMessages entry.
[[nodiscard]] inline bool isBenignQtMessage(std::string_view message) noexcept
{
    return std::ranges::any_of(BenignQtMessages, [message](BenignQtMessage const& entry) {
        return message.find(entry.substring) != std::string_view::npos;
    });
}

} // namespace contour
