// SPDX-License-Identifier: Apache-2.0
#ifdef __linux__
    #include <contour/platform/DBusSignalSubscription.hpp>
    #include <contour/platform/LauncherProgress.hpp>
    #include <contour/platform/TaskbarProgressSink.hpp>
    #include <contour/test/NotificationFixtures.hpp>

    #include <QtCore/QObject>
    #include <QtCore/QScopeGuard>
    #include <QtDBus/QDBusConnection>
    #include <QtGui/QGuiApplication>

    #include <catch2/catch_test_macros.hpp>

    #include <array>

    #include <QtTest/QTest>

namespace
{
/// Captures the actual D-Bus wire payload, independently of the property builder.
class LauncherProgressReceiver final: public QObject
{
    Q_OBJECT
  public:
    QString uri;
    QVariantMap properties;
    int updates = 0;

  public slots:
    void onUpdate(QString const& application, QVariantMap const& values)
    {
        uri = application;
        properties = values;
        ++updates;
    }
};
} // namespace

TEST_CASE("Launcher adapter stays disconnected offscreen", "[contour][taskbar-native]")
{
    if (QGuiApplication::platformName() != QStringLiteral("offscreen"))
        SKIP("Requires the offscreen Qt platform.");
    // The runner's session-bus guard checks construction, updates and destruction for transport access.
    auto sink = contour::platform::makeTaskbarProgressSink();
    CHECK_FALSE(sink->isSupported());
    sink->setApplicationProgress({ .state = vtbackend::ProgressState::Normal, .percentage = 60 });
}

TEST_CASE("Launcher adapter broadcasts progress and clears only what it published",
          "[contour][taskbar-native][display]")
{
    using namespace contour::platform;
    if (!contour::test::onPrivateSessionBus())
        SKIP("Requires a private session bus.");
    auto const platform = QGuiApplication::platformName();
    if (platform != QStringLiteral("xcb") && platform != QStringLiteral("wayland"))
        SKIP("Requires a desktop Qt platform (CONTOUR_TEST_DISPLAY=1).");
    auto sink = makeTaskbarProgressSink();
    if (!sink->isSupported())
        SKIP("Requires a desktop Qt platform and a session bus (CONTOUR_TEST_DISPLAY=1).");
    auto bus = QDBusConnection::sessionBus();
    auto const previousName = QGuiApplication::desktopFileName();
    auto const restoreName = qScopeGuard([&] { QGuiApplication::setDesktopFileName(previousName); });
    QGuiApplication::setDesktopFileName(QStringLiteral("contour-progress-test"));
    LauncherProgressReceiver receiver;
    auto const subscriptions = std::array { DBusSignalSubscription { QLatin1StringView("Update"),
                                                                     SLOT(onUpdate(QString, QVariantMap)) } };
    auto const guard =
        subscribeToDBusSignals(bus,
                               { {},
                                 QLatin1StringView("/org/contourterminal/Contour/LauncherEntry"),
                                 QLatin1StringView("com.canonical.Unity.LauncherEntry") },
                               &receiver,
                               subscriptions);
    CHECK(sink->scope() == TaskbarProgressScope::Application);
    // An adapter that never published clears nothing: a stray Update would arrive first on the bus and
    // fail the progress check below.
    makeTaskbarProgressSink().reset();
    sink->setApplicationProgress({ .state = vtbackend::ProgressState::Normal, .percentage = 60 });
    REQUIRE(QTest::qWaitFor([&] { return receiver.updates == 1; }, 1000));
    CHECK(receiver.uri == QStringLiteral("application://contour-progress-test.desktop"));
    CHECK(receiver.properties.value(QStringLiteral("progress")).metaType() == QMetaType::fromType<double>());
    CHECK(receiver.properties.value(QStringLiteral("progress")).toDouble() == 0.6);
    CHECK(receiver.properties.value(QStringLiteral("progress-visible")).toBool());
    sink.reset();
    REQUIRE(QTest::qWaitFor([&] { return receiver.updates == 2; }, 1000));
    CHECK_FALSE(receiver.properties.value(QStringLiteral("progress-visible")).toBool());
}

    #include "LauncherProgressSink_test.moc"
#endif
