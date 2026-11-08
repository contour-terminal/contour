// SPDX-License-Identifier: Apache-2.0
#include <vtpty/SpawnSpec.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <ranges>

using namespace vtpty;
using namespace std::string_literals;

namespace
{
SpawnSpec fullSpec()
{
    auto slots = TrampolineSlots {};
    for (auto const slot: { TrampolineSlot::Gate,
                            TrampolineSlot::Settle,
                            TrampolineSlot::Terminal,
                            TrampolineSlot::FastPipe,
                            TrampolineSlot::Input,
                            TrampolineSlot::Output })
        slots = slots.with(slot);
    return SpawnSpec {
        .mode = SpawnMode::Detached,
        .image = "/usr/bin/my prog",
        .argv = { "prog",
                  "with space",
                  "gr\xC3\xBC\xC3\x9F"
                  "e",
                  "" },
        .lookup = ExecLookup::SearchPath,
        .fallbackArgv = { "-sh", "-l" },
        .fallbackText = "/bin/sh -l",
        .workingDirectory = "/tmp/w\xC3\xB6rk",
        .environment = std::vector<std::string> { "A=b=c", "EMPTY=", "TERM=xterm" },
        .oomScoreAdjust = -17,
        .controllingTerminal = ControllingTerminal::Acquire,
        .terminalSettings = TerminalSettings::Configure,
        .failureReport = FailureReport::OnTerminal,
        .inheritedSlots = slots,
    };
}
} // namespace

TEST_CASE("SpawnSpec.round-trips every field", "[vtpty][spawnspec]")
{
    auto const spec = fullSpec();
    auto const decoded = decodeSpawnSpec(encodeSpawnSpec(spec));
    REQUIRE(decoded.has_value());
    CHECK(*decoded == spec);
}

TEST_CASE("SpawnSpec.round-trips the defaults", "[vtpty][spawnspec]")
{
    auto const spec = SpawnSpec { .argv = { "/bin/true" } };
    auto const decoded = decodeSpawnSpec(encodeSpawnSpec(spec));
    REQUIRE(decoded.has_value());
    CHECK(*decoded == spec);
    CHECK_FALSE(decoded->environment.has_value());
}

TEST_CASE("SpawnSpec.keeps an image distinct from argv[0]", "[vtpty][spawnspec]")
{
    auto const spec = SpawnSpec { .image = "/usr/bin/tmux", .argv = { "tmux", "-C" } };
    auto const decoded = decodeSpawnSpec(encodeSpawnSpec(spec));
    REQUIRE(decoded.has_value());
    CHECK(decoded->image == "/usr/bin/tmux");
    CHECK(decoded->argv == std::vector<std::string> { "tmux", "-C" });
}

TEST_CASE("SpawnSpec.rejects truncated input", "[vtpty][spawnspec]")
{
    auto const encoded = encodeSpawnSpec(fullSpec());
    for (auto const length: std::views::iota(std::size_t { 0 }, encoded.size()))
    {
        auto const decoded = decodeSpawnSpec(std::string_view { encoded }.substr(0, length));
        REQUIRE_FALSE(decoded.has_value());
        CHECK(decoded.error() == SpawnSpecError::Truncated);
    }
}

TEST_CASE("SpawnSpec.rejects trailing bytes", "[vtpty][spawnspec]")
{
    auto const encoded = encodeSpawnSpec(fullSpec()) + "x";
    auto const decoded = decodeSpawnSpec(encoded);
    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error() == SpawnSpecError::TrailingBytes);
}

TEST_CASE("SpawnSpec.rejects an unknown enumerator", "[vtpty][spawnspec]")
{
    auto encoded = encodeSpawnSpec(fullSpec());
    encoded[0] = static_cast<char>(0xFF);
    auto const decoded = decodeSpawnSpec(encoded);
    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error() == SpawnSpecError::UnknownEnumerator);
}

TEST_CASE("SpawnSpec.rejects slot bits past the last slot", "[vtpty][spawnspec]")
{
    auto encoded = encodeSpawnSpec(fullSpec());
    encoded.back() = static_cast<char>(0xFF);
    auto const decoded = decodeSpawnSpec(encoded);
    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error() == SpawnSpecError::UnknownEnumerator);
}

TEST_CASE("SpawnSpec.rejects an empty argv", "[vtpty][spawnspec]")
{
    auto const decoded = decodeSpawnSpec(encodeSpawnSpec(SpawnSpec {}));
    REQUIRE_FALSE(decoded.has_value());
    CHECK(decoded.error() == SpawnSpecError::EmptyArgv);
}

TEST_CASE("SpawnSpec.classifyInvocation", "[vtpty][spawnspec]")
{
    auto const classify = [](std::initializer_list<char const*> args) {
        auto const argv = std::vector<char const*> { args };
        return classifyInvocation(argv);
    };
    CHECK(classify({ "contour" }) == TrampolineInvocation::NotRequested);
    CHECK(classify({ "x", "--vtpty-spawn-trampoline=1" }) == TrampolineInvocation::Supported);
    CHECK(classify({ "x", "--vtpty-spawn-trampoline=2" }) == TrampolineInvocation::UnsupportedVersion);
    CHECK(classify({ "x", "--vtpty-spawn-trampoline=" }) == TrampolineInvocation::UnsupportedVersion);
    CHECK(classify({ "x", "--vtpty-spawn-trampoline=1", "extra" }) == TrampolineInvocation::Supported);
    CHECK(classify({ "x", "other" }) == TrampolineInvocation::NotRequested);
}

TEST_CASE("SpawnSpec.trampolineFd", "[vtpty][spawnspec]")
{
    CHECK(trampolineFd(TrampolineSlot::Gate) == 4);
    CHECK(trampolineFd(TrampolineSlot::Output) == 9);
    for (auto const slot: { TrampolineSlot::Gate, TrampolineSlot::Output, TrampolineSlot::Terminal })
        CHECK(trampolineFd(slot) != SpawnSpecFd);
}
