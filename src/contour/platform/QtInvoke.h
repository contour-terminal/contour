// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QtCore/QObject>
#include <QtCore/Qt>

#include <utility>

namespace contour::platform
{

/// Posts a functor to be executed on the event loop of the thread that owns @p obj.
///
/// Uses QMetaObject::invokeMethod with Qt::QueuedConnection to ensure the functor
/// is invoked via a QMetaCallEvent, which Qt's event loop processes promptly.
template <typename F>
void postToObject(QObject* obj, F fun)
{
    QMetaObject::invokeMethod(obj, std::forward<F>(fun), Qt::QueuedConnection);
}

} // namespace contour::platform
