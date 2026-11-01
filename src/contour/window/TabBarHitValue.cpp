// SPDX-License-Identifier: Apache-2.0
#include <contour/window/NodeDrag.hpp>
#include <contour/window/TabBarHitValue.hpp>

#include <QtQml/QJSEngine>
#include <QtQml/QQmlEngine>

namespace contour::window
{

void registerTabBarQmlTypes()
{
    // Function-local, so the first call registers and every later one returns: the application registers
    // once at start, a test once it needs the names.
    static auto const registered = [] {
        qmlRegisterUncreatableMetaObject(tabbar::staticMetaObject,
                                         "Contour.Terminal",
                                         1,
                                         0,
                                         "TabBarSpot",
                                         QStringLiteral("TabBarSpot holds enums only."));
        // The mime type a pane or tab drag carries, named once: every drag source and drop area reads it as
        // NodeDrag.mime.
        qmlRegisterSingletonType("Contour.Terminal", 1, 0, "NodeDrag", [](QQmlEngine*, QJSEngine* engine) {
            auto format = engine->newObject();
            format.setProperty(QStringLiteral("mime"), QString::fromLatin1(NodeDragMime));
            return format;
        });
        return true;
    }();
    (void) registered;
}

} // namespace contour::window
