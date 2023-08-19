// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QtGui/QWindow>

namespace BlurBehind
{

void setEnabled(QWindow* window, bool enabled, QRegion region = QRegion());

}
