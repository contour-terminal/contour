// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/Clipboard.h>

#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>

namespace contour::platform
{

void copyToClipboard(std::string_view data)
{
    if (QClipboard* clipboard = QGuiApplication::clipboard(); clipboard != nullptr)
        clipboard->setText(QString::fromUtf8(data.data(), static_cast<int>(data.size())));
}

} // namespace contour::platform
