// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QtGui/QWindow>

namespace contour::platform
{

void setBlurBehind(QWindow* window, bool enabled, QRegion const& region = QRegion());

}
