// SPDX-License-Identifier: Apache-2.0
#include <contour/session/Permission.hpp>

#include <QtQml/QQmlEngine>

namespace contour::session
{

void registerPermissionQmlTypes()
{
    // Function-local, so the first call registers and every later one returns: the application registers
    // once at start, the test runner once after its application object is built.
    static auto const registered = [] {
        qmlRegisterUncreatableMetaObject(permission::staticMetaObject,
                                         "Contour.Terminal",
                                         1,
                                         0,
                                         "Permission",
                                         QStringLiteral("Permission holds enums only."));
        return true;
    }();
    (void) registered;
}

} // namespace contour::session
