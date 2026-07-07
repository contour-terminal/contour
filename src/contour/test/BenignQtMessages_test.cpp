// SPDX-License-Identifier: Apache-2.0
//
// Data-driven test for the benign-Qt-message filter (isBenignQtMessage). The predicate decides which
// Qt-internal log messages the app's message handler (main.cpp) drops before they reach the user;
// testing it directly verifies that decision without installing a Qt message handler.

#include <contour/BenignQtMessages.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <string>
#include <string_view>

using contour::isBenignQtMessage;

TEST_CASE("isBenignQtMessage.dropsKnownBenignNoise", "[contour][qt-log]")
{
    // Real message shapes as Qt emits them; each must be recognized as benign regardless of surrounding text.
    auto const message = GENERATE(
        std::string_view("Ignoring sourceSize request for image url that came from grabToImage. Use the "
                         "targetSize parameter of the grabToImage() function instead."),
        // Substring match is position-independent: the same phrase embedded in other framing still matches.
        std::string_view("prefix :: came from grabToImage :: suffix"));

    CAPTURE(std::string { message });
    CHECK(isBenignQtMessage(message));
}

TEST_CASE("isBenignQtMessage.keepsRealDiagnostics", "[contour][qt-log]")
{
    // Messages that are NOT in the benign table must pass through (return false) so real diagnostics survive.
    auto const message = GENERATE(
        std::string_view(""),
        std::string_view("qrc:/contour/ui/main.qml:42: ReferenceError: terminalSessions is not defined"),
        std::string_view("freetype: Failed to set LCD filter. unimplemented feature"),
        std::string_view("qt.qpa.fonts: Populating font family aliases took 98 ms."),
        // Mentions grabToImage but not the suppressed phrase -> must still be printed.
        std::string_view("grabToImage() failed: item has no window"));

    CAPTURE(std::string { message });
    CHECK(!isBenignQtMessage(message));
}
