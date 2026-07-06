// SPDX-License-Identifier: Apache-2.0
//
// Data-driven test for the QML-diagnostic gate predicate (isQmlDiagnostic). The predicate is the oracle the
// run-wide gate in test_main.cpp uses to fail the whole run on QML defects; testing it directly verifies the
// oracle without emitting a live diagnostic (which would fail the run-wide gate itself).

#include <contour/test/QmlMessageCapture.h>

#include <QtCore/QString>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using contour::test::isQmlDiagnostic;

TEST_CASE("isQmlDiagnostic.matchesQmlDefects", "[contour][qml-gate]")
{
    // Real message shapes as Qt emits them through the message handler.
    auto const message = GENERATE(
        // Source-located diagnostics (component load errors, warnings with a .qml/qrc origin):
        QStringLiteral("qrc:/contour/ui/main.qml:42: ReferenceError: terminalSessions is not defined"),
        QStringLiteral("file:///tmp/Foo.qml:7:5: Unable to assign null to QQuickItem*"),
        QStringLiteral("qrc:/contour/ui/PaneNode.qml:76:9: QML SplitView: Binding loop detected for "
                       "property \"preferredWidth\""),
        // JS error classes without a source suffix in the body:
        QStringLiteral("ReferenceError: win is not defined"),
        QStringLiteral("TypeError: Cannot read property 'width' of null"),
        // Binding loop reported from a QQmlComponent::setData()-created component: the context is
        // "<Unknown File>" — neither ".qml" nor "qrc:/" appears anywhere in the message.
        QStringLiteral("<Unknown File>: QML Item: Binding loop detected for property \"width\""),
        QStringLiteral("<Unknown File>:12:5: QML Binding: Binding loop detected for property \"value\""));

    CAPTURE(message.toStdString());
    CHECK(isQmlDiagnostic(message));
}

TEST_CASE("isQmlDiagnostic.ignoresLibraryChatter", "[contour][qml-gate]")
{
    // Unrelated warning/critical chatter observed on developer machines and CI runners; none of these are
    // QML defects and none may trip the gate.
    auto const message = GENERATE(
        QStringLiteral("libva info: VA-API version 1.20.0"),
        QStringLiteral("libva error: /usr/lib64/dri/iHD_drv_video.so init failed"),
        QStringLiteral("[ffmpeg] deprecated pixel format used, make sure you did set range correctly"),
        QStringLiteral("qt.qpa.fonts: Populating font family aliases took 220 ms."),
        QStringLiteral("QRhiGles2: Failed to create temporary context"),
        // Mentions "loop" without being a binding loop:
        QStringLiteral("event loop already running"));

    CAPTURE(message.toStdString());
    CHECK(!isQmlDiagnostic(message));
}
