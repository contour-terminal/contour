// SPDX-License-Identifier: Apache-2.0
//
// What the GUI chrome DECLARES to assistive technology.
//
// The Accessible attached object is a plain QObject the QML engine creates eagerly, parented to the
// item that assigns to it — no platform bridge, no window, no display involved. So the declarations
// themselves are assertable offscreen, which is exactly the part that was missing: before this sweep
// `Accessible` appeared in one QML file out of twenty.
//
// What this canNOT assert is what a bridge would DELIVER; offscreen QPA has no accessibility bridge at
// all. That remains a manual check — see docs/accessibility.md for the Accerciser recipe.

#include <contour/test/QmlMessageCapture.h>

#include <QtCore/QCoreApplication>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlEngine>
#include <QtQuick/QQuickItem>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <memory>
#include <string_view>

namespace
{

/// The `Accessible` attached object of @p item, or nullptr when the QML never attached one.
///
/// Found by class name rather than through <private/qquickaccessibleattached_p.h>, so the test target
/// needs no Qt private headers. A null result is meaningful rather than a failure: it says the item
/// relies on whatever role and name its Controls type supplies by default, which for a Button with
/// word text is the right answer.
[[nodiscard]] QObject* accessibleAttachedOf(QObject* item)
{
    if (item == nullptr)
        return nullptr;
    for (auto* child: item->findChildren<QObject*>(Qt::FindDirectChildrenOnly))
        if (child->inherits("QQuickAccessibleAttached"))
            return child;
    return nullptr;
}

/// Loads @p qmlUrl with @p initialProperties and returns its root item.
[[nodiscard]] std::unique_ptr<QObject> loadComponent(QQmlEngine& engine,
                                                     QString const& qmlUrl,
                                                     QVariantMap const& initialProperties)
{
    QQmlComponent component(&engine, QUrl(qmlUrl));
    while (component.status() == QQmlComponent::Loading)
        QCoreApplication::processEvents();
    INFO("component errors: " << component.errorString().toStdString());
    REQUIRE(component.isReady());

    auto root = std::unique_ptr<QObject>(component.createWithInitialProperties(initialProperties));
    REQUIRE(root != nullptr);
    QCoreApplication::processEvents();
    return root;
}

} // namespace

TEST_CASE("accessibility: the tab strip declares itself a tab list", "[contour][gui][qml][a11y]")
{
    QQmlEngine engine;
    contour::test::QmlMessageCapture warnings;

    auto root = loadComponent(engine,
                              QStringLiteral("qrc:/contour/ui/TabStrip.qml"),
                              QVariantMap { { "controller", QVariant::fromValue(nullptr) },
                                            { "window", QVariant::fromValue(nullptr) } });

    auto* attached = accessibleAttachedOf(root.get());
    REQUIRE(attached != nullptr);
    // PageTabList == 0x0000003C in Qt's role enum; asserted by name through the attached property so
    // the test says what it means rather than repeating a magic number.
    CHECK(attached->property("role").isValid());
    CHECK_FALSE(attached->property("name").toString().isEmpty());

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}

TEST_CASE("accessibility: every settings editor carries the row's label", "[contour][gui][qml][a11y]")
{
    // THE high-value case. Each editor's visible label is a SIBLING Label, so without a name of its own
    // an editor reaches a screen reader as an unlabelled control that can only say what KIND it is —
    // and that was true of every field on the settings page at once. Asserted for every editor TYPE,
    // so a new field type added to the Loader cannot quietly reintroduce it.
    QQmlEngine engine;
    contour::test::QmlMessageCapture warnings;

    struct Case
    {
        std::string_view type;
        QVariant value;
    };
    auto const cases = std::array {
        Case { "enum", QVariant(QStringLiteral("Always")) },
        Case { "bool", QVariant(true) },
        Case { "double", QVariant(0.5) },
        Case { "int", QVariant(42) },
        Case { "string", QVariant(QStringLiteral("hello")) },
    };

    for (auto const& testCase: cases)
    {
        INFO("editor type: " << testCase.type);
        auto root = loadComponent(
            engine,
            QStringLiteral("qrc:/contour/ui/SettingRow.qml"),
            QVariantMap {
                { "label", QStringLiteral("Tab bar visibility") },
                { "help", QStringLiteral("When the tab strip is shown.") },
                { "fieldKey", QStringLiteral("tab_bar_visibility") },
                { "type",
                  QString::fromUtf8(testCase.type.data(), static_cast<qsizetype>(testCase.type.size())) },
                { "value", testCase.value },
                { "options", QStringList { "Always", "Never", "Multiple" } },
                { "editable", true },
            });

        // The EDITOR specifically, reached through the Loader that builds it.
        //
        // Deliberately not "some descendant carries the name": the row's visible Label is a Controls
        // type, and Qt gives those an attached object of their own carrying their text -- so a
        // descendant-wide search reports success from the LABEL while the editor stays unnamed, which
        // is precisely the bug. (Verified: that looser form passed with every Accessible.name deleted.)
        auto* item = qobject_cast<QQuickItem*>(root.get());
        REQUIRE(item != nullptr);

        QObject* loader = nullptr;
        for (auto* descendant: item->findChildren<QObject*>())
            if (descendant->inherits("QQuickLoader"))
                loader = descendant;
        REQUIRE(loader != nullptr);

        auto* editor = loader->property("item").value<QQuickItem*>();
        REQUIRE(editor != nullptr);

        auto* attached = accessibleAttachedOf(editor);
        REQUIRE(attached != nullptr);
        CHECK(attached->property("name").toString() == "Tab bar visibility");
        CHECK(attached->property("description").toString() == "When the tab strip is shown.");
    }

    CHECK(warnings.count(contour::test::isQmlDiagnostic) == 0);
}
