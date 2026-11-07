// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/Clipboard.hpp>

#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>

#include <array>
#include <utility>

namespace contour::platform
{

namespace
{
    /// The Qt clipboard mode each target is written through.
    constexpr auto TargetModes = std::array {
        std::pair { vtbackend::ClipboardTarget::Clipboard, QClipboard::Clipboard },
        std::pair { vtbackend::ClipboardTarget::PrimarySelection, QClipboard::Selection },
    };
} // namespace

void copyToClipboard(std::string_view data, vtbackend::ClipboardTargets targets)
{
    QClipboard* clipboard = QGuiApplication::clipboard();
    if (clipboard == nullptr)
        return;

    auto const support =
        clipboard->supportsSelection() ? PrimarySelectionSupport::Present : PrimarySelectionSupport::Absent;
    auto const reachable = reachableTargets(targets, support);
    if (reachable.none())
        return; // e.g. a primary-selection write on macOS: nothing to convert the text for
    auto const text = QString::fromUtf8(data.data(), static_cast<qsizetype>(data.size()));
    for (auto const& [target, mode]: TargetModes)
        if (reachable.test(target))
            clipboard->setText(text, mode);
}

} // namespace contour::platform
