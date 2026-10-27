// SPDX-License-Identifier: Apache-2.0
#include <contour/session/Permission.hpp>
#include <contour/test/QmlMessageCapture.hpp>

#include <QtCore/QMetaEnum>
#include <QtCore/QObject>
#include <QtCore/QVariantMap>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlContext>
#include <QtQml/QQmlEngine>

#include <catch2/catch_test_macros.hpp>

#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <utility>

using contour::session::PermissionDecision;
using contour::session::PermissionMemory;

namespace
{

/// Takes what QML hands a Q_INVOKABLE spelled in the permission enums, and offers one back as a property.
/// The parameters name the declaring namespace, as moc's normalized signature must to find the registered
/// type.
class PermissionEcho: public QObject
{
    Q_OBJECT
    Q_PROPERTY(contour::session::permission::PermissionMemory memory READ memory CONSTANT)
    Q_PROPERTY(QVariantMap prompt READ prompt CONSTANT)

  public:
    /// The memory QML reads back: the one no default would produce.
    [[nodiscard]] contour::session::permission::PermissionMemory memory() const noexcept
    {
        return PermissionMemory::Persistent;
    }

    /// An enum inside a QVariantMap, as the session's pendingPermission carries one: its underlying int.
    [[nodiscard]] QVariantMap prompt() const
    {
        return { { QStringLiteral("initialMemory"), std::to_underlying(PermissionMemory::Session) } };
    }

    /// Records the answer QML gave.
    Q_INVOKABLE void take(contour::session::permission::PermissionDecision decision,
                          contour::session::permission::PermissionMemory memory)
    {
        taken = std::pair { decision, memory };
    }

    std::optional<std::pair<PermissionDecision, PermissionMemory>> taken;
};

/// Evaluates @p expression in a QML engine that has imported Contour.Terminal.
[[nodiscard]] QVariant evaluateQml(QQmlEngine& engine, QString const& expression)
{
    auto component = QQmlComponent { &engine };
    component.setData(
        QStringLiteral("import QtQuick; import Contour.Terminal; QtObject { property var value: %1 }")
            .arg(expression)
            .toUtf8(),
        QUrl {});
    auto const object = std::unique_ptr<QObject> { component.create() };
    REQUIRE(object != nullptr);
    return object->property("value");
}

/// Every enumerator of @p E with its key, as Qt's meta-object lists them.
template <typename E>
[[nodiscard]] auto enumerators()
{
    auto const meta = QMetaEnum::fromType<E>();
    return std::views::iota(0, meta.keyCount()) | std::views::transform([meta](int index) {
               return std::pair { QString::fromLatin1(meta.key(index)), meta.value(index) };
           });
}

} // namespace

TEST_CASE("Permission: the permission enums round-trip through QML", "[contour][permission][qml]")
{
    contour::session::registerPermissionQmlTypes();
    auto const capture = contour::test::QmlMessageCapture {};
    auto engine = QQmlEngine {};
    auto echo = PermissionEcho {};
    engine.rootContext()->setContextProperty(QStringLiteral("echo"), &echo);

    auto component = QQmlComponent { &engine };
    component.setData(R"(
        import QtQuick
        import Contour.Terminal
        QtObject {
            property bool sawPersistent: echo.memory === Permission.Persistent
            property bool sawSessionInMap: echo.prompt.initialMemory === Permission.Session
            Component.onCompleted: echo.take(Permission.Allow, Permission.Session)
        }
    )",
                      QUrl {});
    auto const object = std::unique_ptr<QObject> { component.create() };
    INFO(component.errorString().toStdString());
    REQUIRE(object != nullptr);

    REQUIRE(echo.taken.has_value());
    CHECK(echo.taken->first == PermissionDecision::Allow);
    CHECK(echo.taken->second == PermissionMemory::Session);
    CHECK(object->property("sawPersistent").toBool());
    CHECK(object->property("sawSessionInMap").toBool());
    CHECK(capture.messages().isEmpty());
}

TEST_CASE("Permission: each enumerator keeps its value in QML", "[contour][permission][qml]")
{
    contour::session::registerPermissionQmlTypes();
    auto const capture = contour::test::QmlMessageCapture {};
    auto engine = QQmlEngine {};

    auto const checkAll = [&]<typename E>() {
        for (auto const& [key, value]: enumerators<E>())
        {
            INFO(key.toStdString());
            CHECK(evaluateQml(engine, QStringLiteral("Permission.") + key).toInt() == value);
        }
    };
    checkAll.operator()<PermissionDecision>();
    checkAll.operator()<PermissionMemory>();
    CHECK(capture.messages().isEmpty());
}

TEST_CASE("Permission: the log spells each value", "[contour][permission][qml]")
{
    CHECK(std::format("{}", PermissionDecision::Deny) == "deny");
    CHECK(std::format("{}", PermissionDecision::Allow) == "allow");
    CHECK(std::format("{}", PermissionMemory::Once) == "once");
    CHECK(std::format("{}", PermissionMemory::Session) == "session");
    CHECK(std::format("{}", PermissionMemory::Persistent) == "persistent");
}

#include <Permission_test.moc>
