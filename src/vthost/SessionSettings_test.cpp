// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <variant>

#include <vthost/SessionSettings.hpp>

using namespace vthost;

namespace
{

/// The finite scrollback @p settings carries, for a test that already knows it is finite.
[[nodiscard]] int finiteHistory(vtbackend::Settings const& settings)
{
    REQUIRE(std::holds_alternative<vtbackend::LineCount>(settings.historyLimits.capacity));
    return unbox<int>(std::get<vtbackend::LineCount>(settings.historyLimits.capacity));
}

/// Settings that differ from vtbackend's defaults in every field the wire carries, so a round trip
/// that silently dropped one would show up as a mismatch rather than as a coincidence.
[[nodiscard]] vtbackend::Settings distinctiveSettings()
{
    auto settings = vtbackend::Settings {};
    settings.historyLimits = vtbackend::LineCount(4242);
    settings.terminalId = vtbackend::VTType::VT420;
    settings.graphemeClustering = false;
    settings.primaryScreen.allowReflowOnResize = false;
    settings.maxImageRegisterCount = 1024;
    settings.wordDelimiters = U" /\\()";
    settings.frozenModes[vtbackend::DECMode::Unicode] = true;
    settings.frozenModes[vtbackend::DECMode::BlinkingCursor] = false;
    return settings;
}

} // namespace

TEST_CASE("hostedSessionSettings raises a zero scrollback to the default", "[vthost][settings]")
{
    // The whole point of the helper: a bare vtbackend::Settings is LineCount(0), which is a valid
    // terminal and a broken session HOST.
    auto const bare = vtbackend::Settings {};
    REQUIRE(finiteHistory(bare) == 0);
    CHECK(finiteHistory(hostedSessionSettings(bare)) == DefaultSessionHistoryLineCount);
    CHECK(finiteHistory(defaultSessionSettings()) == DefaultSessionHistoryLineCount);
}

TEST_CASE("hostedSessionSettings caps an absurd finite scrollback", "[vthost][settings]")
{
    auto asked = vtbackend::Settings {};
    asked.historyLimits = vtbackend::LineCount(MaxSessionHistoryLineCount * 10);
    CHECK(finiteHistory(hostedSessionSettings(asked)) == MaxSessionHistoryLineCount);
}

TEST_CASE("hostedSessionSettings leaves a real scrollback alone", "[vthost][settings]")
{
    SECTION("a finite value in range")
    {
        auto asked = vtbackend::Settings {};
        asked.historyLimits = vtbackend::LineCount(10'000);
        CHECK(finiteHistory(hostedSessionSettings(asked)) == 10'000);
    }

    SECTION("a value SMALLER than the default")
    {
        // The default is a default, not a floor. A profile that deliberately keeps 200 lines is not
        // the broken zero-scrollback configuration, so nothing here may "helpfully" raise it.
        auto asked = vtbackend::Settings {};
        asked.historyLimits = vtbackend::LineCount(200);
        CHECK(finiteHistory(hostedSessionSettings(asked)) == 200);
    }

    SECTION("unlimited")
    {
        // Infinite is NOT capped: it is a choice the configuration offers outright, so clamping it
        // would silently overrule a user who asked for it in as many words.
        auto asked = vtbackend::Settings {};
        asked.historyLimits = vtbackend::Infinite {};
        auto const hosted = hostedSessionSettings(asked);
        CHECK(std::holds_alternative<vtbackend::Infinite>(hosted.historyLimits.capacity));
    }
}

TEST_CASE("hostedSessionSettings always enables the good image protocol", "[vthost][settings]")
{
    // Daemon mode adopts the end state the knob is heading for, which is also why it is absent from
    // WireSessionSettings. The day it becomes unconditional, THIS is the test that says so already.
    auto refused = vtbackend::Settings {};
    refused.goodImageProtocol = false;
    CHECK(hostedSessionSettings(refused).goodImageProtocol);

    auto asked = vtbackend::Settings {};
    asked.goodImageProtocol = true;
    CHECK(hostedSessionSettings(asked).goodImageProtocol);
}

TEST_CASE("hostedSessionSettings bounds the per-session allocations", "[vthost][settings]")
{
    SECTION("image registers")
    {
        auto asked = vtbackend::Settings {};
        asked.maxImageRegisterCount = MaxSessionImageRegisterCount * 4;
        CHECK(hostedSessionSettings(asked).maxImageRegisterCount == MaxSessionImageRegisterCount);

        asked.maxImageRegisterCount = 0;
        CHECK(hostedSessionSettings(asked).maxImageRegisterCount == 1);

        asked.maxImageRegisterCount = 512;
        CHECK(hostedSessionSettings(asked).maxImageRegisterCount == 512);
    }

    SECTION("word delimiters")
    {
        auto asked = vtbackend::Settings {};
        asked.wordDelimiters = std::u32string(std::size_t { MaxSessionWordDelimiters } * 3, U'x');
        CHECK(hostedSessionSettings(asked).wordDelimiters.size() == MaxSessionWordDelimiters);

        asked.wordDelimiters = U" /\\()";
        CHECK(hostedSessionSettings(asked).wordDelimiters == U" /\\()");
    }
}

TEST_CASE("session settings round-trip through the wire", "[vthost][settings]")
{
    auto const original = distinctiveSettings();
    auto const restored = fromWireSessionSettings(toWireSessionSettings(original), vtbackend::Settings {});

    CHECK(finiteHistory(restored) == finiteHistory(original));
    CHECK(restored.terminalId == original.terminalId);
    CHECK(restored.graphemeClustering == original.graphemeClustering);
    CHECK(restored.primaryScreen.allowReflowOnResize == original.primaryScreen.allowReflowOnResize);
    CHECK(restored.maxImageRegisterCount == original.maxImageRegisterCount);
    CHECK(restored.wordDelimiters == original.wordDelimiters);
    CHECK(restored.frozenModes == original.frozenModes);
}

TEST_CASE("an unlimited scrollback survives the wire as unlimited", "[vthost][settings]")
{
    // -1 is the configuration's own spelling of `history.limit: unlimited`; a second encoding here
    // would be one more thing to keep in step with it.
    auto asked = vtbackend::Settings {};
    asked.historyLimits = vtbackend::Infinite {};
    auto const wire = toWireSessionSettings(asked);
    CHECK(wire.historyLineCount == -1);
    CHECK(std::holds_alternative<vtbackend::Infinite>(
        fromWireSessionSettings(wire, vtbackend::Settings {}).historyLimits.capacity));
}

TEST_CASE("fromWireSessionSettings falls back to the daemon's own settings", "[vthost][settings]")
{
    // Everything the wire does not carry must come from `base`, not from a wire default: the daemon
    // resolved a profile, and a client stating a preference about scrollback is not thereby stating
    // one about the terminal's status line or its PTY buffer sizes.
    auto base = distinctiveSettings();
    base.allowClipboardRead = true;
    base.statusDisplayType = vtbackend::StatusDisplayType::Indicator;
    base.ptyReadBufferSize = 8192;

    auto wire = proto::WireSessionSettings {};
    wire.historyLineCount = 777;

    auto const applied = fromWireSessionSettings(wire, base);
    CHECK(finiteHistory(applied) == 777);
    CHECK(applied.allowClipboardRead);
    CHECK(applied.statusDisplayType == vtbackend::StatusDisplayType::Indicator);
    CHECK(applied.ptyReadBufferSize == 8192);
}

TEST_CASE("fromWireSessionSettings keeps the base terminalId for an unknown number", "[vthost][settings]")
{
    // VTType's numbering is sparse and not ordered by capability, so an unrecognised number cannot
    // be clamped into something sensible -- keeping the daemon's is the only honest answer.
    auto base = vtbackend::Settings {};
    base.terminalId = vtbackend::VTType::VT340;

    auto wire = proto::WireSessionSettings {};
    wire.terminalId = 42; // no VTType uses 42
    CHECK(fromWireSessionSettings(wire, base).terminalId == vtbackend::VTType::VT340);

    wire.terminalId = 65; // VT525
    CHECK(fromWireSessionSettings(wire, base).terminalId == vtbackend::VTType::VT525);
}

TEST_CASE("fromWireSessionSettings drops frozen modes it cannot name", "[vthost][settings]")
{
    auto wire = proto::WireSessionSettings {};
    wire.frozenModes = {
        proto::WireFrozenMode { .mode = 2027, .frozenAs = 1 },   // DECMode::Unicode
        proto::WireFrozenMode { .mode = 424242, .frozenAs = 1 }, // no such DEC private mode
        proto::WireFrozenMode { .mode = 12, .frozenAs = 0 },     // DECMode::BlinkingCursor
    };

    auto const applied = fromWireSessionSettings(wire, vtbackend::Settings {});
    CHECK(applied.frozenModes.size() == 2);
    REQUIRE(applied.frozenModes.contains(vtbackend::DECMode::Unicode));
    CHECK(applied.frozenModes.at(vtbackend::DECMode::Unicode));
    REQUIRE(applied.frozenModes.contains(vtbackend::DECMode::BlinkingCursor));
    CHECK_FALSE(applied.frozenModes.at(vtbackend::DECMode::BlinkingCursor));
}

TEST_CASE("a client's frozen modes replace the daemon's, not merge with them", "[vthost][settings]")
{
    // Under "the creating client's profile wins", a client that freezes nothing must get sessions
    // that freeze nothing -- otherwise the daemon's profile leaks into a session the client
    // configured, and the leak is invisible until an application cannot set a mode.
    auto base = vtbackend::Settings {};
    base.frozenModes[vtbackend::DECMode::Unicode] = true;

    auto const applied = fromWireSessionSettings(proto::WireSessionSettings {}, base);
    CHECK(applied.frozenModes.empty());
}

TEST_CASE("a client cannot talk a session out of the host invariants", "[vthost][settings]")
{
    // The wire is remote input. Whatever it asks for goes through hostedSessionSettings, so a
    // client cannot land a session on the pathological zero-scrollback configuration -- nor
    // exhaust the daemon with an absurd one.
    auto zero = proto::WireSessionSettings {};
    zero.historyLineCount = 0;
    CHECK(finiteHistory(fromWireSessionSettings(zero, vtbackend::Settings {}))
          == DefaultSessionHistoryLineCount);

    auto absurd = proto::WireSessionSettings {};
    absurd.historyLineCount = 1'000'000'000'000;
    absurd.maxImageRegisterCount = 0xFFFFFFFF;
    auto const bounded = fromWireSessionSettings(absurd, vtbackend::Settings {});
    CHECK(finiteHistory(bounded) == MaxSessionHistoryLineCount);
    CHECK(bounded.maxImageRegisterCount == MaxSessionImageRegisterCount);
}
