// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/SecureInput.hpp>
#include <contour/platform/SecureInputGate.hpp>
#include <contour/test/SecureInputFixtures.hpp>

#include <catch2/catch_test_macros.hpp>

using contour::platform::SecureInputGate;
using contour::platform::SecureInputState;
using contour::test::RecordingSecureInput;
using contour::test::SecureInputRecord;

TEST_CASE("secureInputGate.enablesOnceForRepeatedOn", "[secureinput]")
{
    // Focus moving between two panes that both wait for a password asks for On twice; the OS must see one
    // enable(), or a later disable() leaves it on (Review Focus 3).
    auto record = SecureInputRecord {};
    auto os = RecordingSecureInput { record };
    auto gate = SecureInputGate { os };
    gate.apply(SecureInputState::On);
    gate.apply(SecureInputState::On);
    CHECK(record.enables == 1);
    CHECK(record.disables == 0);
    CHECK(gate.state() == SecureInputState::On);
}

TEST_CASE("secureInputGate.disablesOnlyWhenOn", "[secureinput]")
{
    auto record = SecureInputRecord {};
    auto os = RecordingSecureInput { record };
    auto gate = SecureInputGate { os };
    CHECK(gate.state() == SecureInputState::Off);

    gate.apply(SecureInputState::Off);
    CHECK(record.enables == 0);
    CHECK(record.disables == 0);

    gate.apply(SecureInputState::On);
    gate.apply(SecureInputState::Off);
    gate.apply(SecureInputState::Off);
    CHECK(record.enables == 1);
    CHECK(record.disables == 1);
    CHECK(gate.state() == SecureInputState::Off);
}

TEST_CASE("secureInputGate.destructorTurnsItOff", "[secureinput]")
{
    // Whatever owns the gate going away -- a manager, the app -- must not leave secure input on (Review
    // Focus 2).
    auto record = SecureInputRecord {};
    auto os = RecordingSecureInput { record };

    SECTION("left on")
    {
        {
            auto gate = SecureInputGate { os };
            gate.apply(SecureInputState::On);
        }
        CHECK(record.enables == 1);
        CHECK(record.disables == 1);
    }

    SECTION("left off")
    {
        {
            auto gate = SecureInputGate { os };
            gate.apply(SecureInputState::On);
            gate.apply(SecureInputState::Off);
        }
        CHECK(record.enables == 1);
        CHECK(record.disables == 1);
    }

    SECTION("never on")
    {
        {
            auto const gate = SecureInputGate { os };
        }
        CHECK(record.enables == 0);
        CHECK(record.disables == 0);
    }
}
