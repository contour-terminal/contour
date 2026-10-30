// SPDX-License-Identifier: Apache-2.0
#ifdef _WIN32
    #include <contour/platform/TaskbarProgressSink.hpp>

    #include <core/log/LogSink.hpp>

    #include <QtGui/QGuiApplication>
    #include <QtGui/QWindow>

    #include <catch2/catch_test_macros.hpp>

    #include <QtTest/QTest>

TEST_CASE("Windows taskbar overlay is retained until the button exists, clears, and frees its icon",
          "[contour][taskbar-native][win32][display]")
{
    using vtbackend::ProgressState;
    if (QGuiApplication::platformName() != QStringLiteral("windows"))
        SKIP("Requires CONTOUR_TEST_DISPLAY=1 and the Windows Qt platform.");
    auto capture = core::log::ScopedCapture { "taskbar.progress" };
    auto sink = contour::platform::makeTaskbarProgressSink();
    QWindow window;
    window.setTitle(QStringLiteral("Contour taskbar overlay smoke test"));
    window.resize(480, 120);
    sink->bindWindow(1, window);

    // Asked for before the native button exists: retained, then published once TaskbarButtonCreated arrives.
    sink->setAttention(1, { .waiting = 2 });
    CHECK_FALSE(capture.text().contains("overlay 2"));
    window.show();
    REQUIRE(QTest::qWaitFor([&] { return capture.text().contains("overlay 2"); }, 5000));
    CHECK(contour::platform::liveOverlayIconCount() == 1);

    // Both calls succeed in either order, and the overlay and the progress bar coexist.
    sink->setProgress(1, { .state = ProgressState::Normal, .percentage = 40 });
    sink->setAttention(1, { .waiting = 3 });
    CHECK(contour::platform::liveOverlayIconCount() == 1); // The previous icon was replaced, not leaked.
    sink->setAttention(1, { .waiting = 0 });
    CHECK(capture.text().contains("overlay 0"));
    CHECK(contour::platform::liveOverlayIconCount() == 0); // Cleared: no icon is kept.

    sink->setAttention(1, { .waiting = 1 });
    CHECK(contour::platform::liveOverlayIconCount() == 1);
    sink->removeWindow(1);
    CHECK(contour::platform::liveOverlayIconCount() == 0);
    CHECK_FALSE(capture.text().contains("failed"));
}
#endif
