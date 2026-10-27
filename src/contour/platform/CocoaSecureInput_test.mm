// SPDX-License-Identifier: Apache-2.0
//
// AppKit's Secure Keyboard Entry, driven through the gate as the app drives it. Needs no window: the call is
// process-wide. Outside a login session (a CI runner without a console user) the OS may not report the
// state, so the OS's answer is checked only where it flipped; the balanced round trip is checked always.

#include <contour/platform/SecureInput.hpp>
#include <contour/platform/SecureInputGate.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>

#import <Carbon/Carbon.h>

using contour::platform::SecureInputGate;
using contour::platform::SecureInputState;
using contour::platform::SecureInputSupport;

TEST_CASE("CocoaSecureInput.isNative", "[secureinput][cocoa]")
{
    CHECK(contour::platform::makeSecureInput()->support() == SecureInputSupport::Native);
}

TEST_CASE("CocoaSecureInput.gateRestoresTheSystemState", "[secureinput][cocoa]")
{
    auto const before = IsSecureEventInputEnabled() != 0;
    auto os = contour::platform::makeSecureInput();
    auto reportedOn = false;
    {
        auto gate = SecureInputGate { *os };
        gate.apply(SecureInputState::On);
        reportedOn = IsSecureEventInputEnabled() != 0;
    }
    CHECK((IsSecureEventInputEnabled() != 0) == before);
    // Destroying the backend after the gate must not disable a second time: the system state still matches.
    os.reset();
    CHECK((IsSecureEventInputEnabled() != 0) == before);
    if (!reportedOn)
        SKIP("The OS did not report Secure Keyboard Entry on (no login session); "
             "only the round trip is checked");
}
