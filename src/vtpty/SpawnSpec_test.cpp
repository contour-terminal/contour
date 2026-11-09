// SPDX-License-Identifier: Apache-2.0
#include <vtpty/SpawnSpec.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <variant>

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
        .image = "/usr/bin/my prog",
        .argv = { "prog",
                  "with space",
                  "gr\xC3\xBC\xC3\x9F"
                  "e",
                  "" },
        .lookup = ExecLookup::SearchPath,
        .environment = std::vector<std::string> { "A=b=c", "EMPTY=", "TERM=xterm" },
        .oomScoreAdjust = -17,
        .inheritedSlots = slots,
        .launch =
            TerminalLaunch {
                .fallbackArgv = { "-sh", "-l" },
                .workingDirectory = "/tmp/w\xC3\xB6rk",
                .controllingTerminal = ControllingTerminal::Acquire,
                .terminalSettings = TerminalSettings::Configure,
                .failureReport = FailureReport::OnTerminal,
                .failureExit = FailureExit::NotFound,
            },
    };
}

/// The smallest spec there is, encoded: a terminal launch of argv { "a" }. Its bytes, by offset:
/// 0 mode | 1-4 image | 5-13 argv | 14 lookup | 15 environment presence | 16 OOM presence | 17 slots |
/// 18-21 fallback argv | 22-25 working directory | 26 controlling terminal | 27 terminal settings |
/// 28 failure report | 29 failure exit.
std::string minimalEncoding()
{
    auto encoded = encodeSpawnSpec(SpawnSpec { .argv = { "a" } });
    REQUIRE(encoded.size() == 30);
    return encoded;
}

/// @return Why @p encoded, with the byte at @p offset replaced by @p value, is not a spec.
std::optional<SpawnSpecError> rejectionOf(std::string encoded, std::size_t offset, std::uint8_t value)
{
    encoded[offset] = static_cast<char>(value);
    auto const decoded = decodeSpawnSpec(encoded);
    if (decoded)
        return std::nullopt;
    return decoded.error();
}
} // namespace

TEST_CASE("SpawnSpec.round-trips every field", "[vtpty][spawnspec]")
{
    auto const spec = fullSpec();
    auto const decoded = decodeSpawnSpec(encodeSpawnSpec(spec));
    REQUIRE(decoded.has_value());
    CHECK(*decoded == spec);
}

TEST_CASE("SpawnSpec.round-trips a detached launch", "[vtpty][spawnspec]")
{
    auto spec = fullSpec();
    spec.launch = DetachedLaunch {};
    auto const decoded = decodeSpawnSpec(encodeSpawnSpec(spec));
    REQUIRE(decoded.has_value());
    CHECK(*decoded == spec);
    CHECK(std::holds_alternative<DetachedLaunch>(decoded->launch));
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

TEST_CASE("SpawnSpec.the minimal encoding decodes", "[vtpty][spawnspec]")
{
    // The baseline the rejection tests below alter one byte of.
    auto const decoded = decodeSpawnSpec(minimalEncoding());
    REQUIRE(decoded.has_value());
    CHECK(*decoded == SpawnSpec { .argv = { "a" } });
}

TEST_CASE("SpawnSpec.rejects an unknown enumerator", "[vtpty][spawnspec]")
{
    auto const encoded = minimalEncoding();
    // Each enumerator byte, set just past its enum's last value.
    SECTION("mode")
    {
        CHECK(rejectionOf(encoded, 0, 2) == SpawnSpecError::UnknownEnumerator);
    }
    SECTION("lookup")
    {
        CHECK(rejectionOf(encoded, 14, 2) == SpawnSpecError::UnknownEnumerator);
    }
    SECTION("controlling terminal")
    {
        CHECK(rejectionOf(encoded, 26, 2) == SpawnSpecError::UnknownEnumerator);
    }
    SECTION("terminal settings")
    {
        CHECK(rejectionOf(encoded, 27, 2) == SpawnSpecError::UnknownEnumerator);
    }
    SECTION("failure report")
    {
        CHECK(rejectionOf(encoded, 28, 2) == SpawnSpecError::UnknownEnumerator);
    }
    SECTION("failure exit")
    {
        CHECK(rejectionOf(encoded, 29, 2) == SpawnSpecError::UnknownEnumerator);
    }
}

TEST_CASE("SpawnSpec.rejects a presence byte that is neither 0 nor 1", "[vtpty][spawnspec]")
{
    auto const encoded = minimalEncoding();
    SECTION("environment")
    {
        CHECK(rejectionOf(encoded, 15, 2) == SpawnSpecError::UnknownEnumerator);
    }
    SECTION("OOM score adjustment")
    {
        CHECK(rejectionOf(encoded, 16, 2) == SpawnSpecError::UnknownEnumerator);
    }
}

TEST_CASE("SpawnSpec.a framed spec is its length, then the spec", "[vtpty][spawnspec]")
{
    auto const spec = fullSpec();
    auto const encoded = encodeSpawnSpec(spec);
    auto const framed = encodeFramedSpawnSpec(spec);
    REQUIRE(framed.size() == SpawnSpecLengthSize + encoded.size());
    CHECK(framed.substr(SpawnSpecLengthSize) == encoded);
    CHECK(decodeSpawnSpecLength(framed) == static_cast<std::uint32_t>(encoded.size()));
    CHECK(decodeSpawnSpecLength(framed.substr(0, 3)) == std::unexpected { SpawnSpecError::Truncated });
}

TEST_CASE("SpawnSpec.rejects slot bits past the last slot", "[vtpty][spawnspec]")
{
    CHECK(rejectionOf(minimalEncoding(), 17, 0xFF) == SpawnSpecError::UnknownEnumerator);
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
