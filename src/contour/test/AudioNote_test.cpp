// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the pure PCM bell synthesis (contour/AudioNote.h), extracted from Audio.cpp's
// anonymous namespace so the waveform math is verifiable without an audio device.

#include <contour/AudioNote.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <span>

using namespace contour::audio;

TEST_CASE("audio: squareWave alternates with period 2", "[audio]")
{
    CHECK(squareWave(0.0) == 1.0);
    CHECK(squareWave(0.5) == 1.0);
    CHECK(squareWave(1.5) == -1.0);
    CHECK(squareWave(2.5) == 1.0); // wraps modulo 2
}

TEST_CASE("audio: noteToFrequency anchors the DECPS scale", "[audio]")
{
    CHECK(noteToFrequency(0) == 0.0);                       // silence
    CHECK(noteToFrequency(10) == Catch::Approx(880.0));     // (10+2)/12 = 1 octave above 440
    CHECK(noteToFrequency(22) == Catch::Approx(1760.0));    // two octaves
    CHECK(noteToFrequency(4) == Catch::Approx(622.253967)); // D#5
}

TEST_CASE("audio: synthesizeNote yields ceil(duration/32*rate) samples scaled by volume", "[audio]")
{
    auto const oneUnit = synthesizeNote(7.0, 1, 880.0); // 1/32s at full DECPS volume
    CHECK(oneUnit.size() == static_cast<size_t>(std::ceil(SampleRate / 32.0)));

    // Full volume (7/7): first sample sits at +0x7fff (squareWave(0) == 1).
    REQUIRE_FALSE(oneUnit.empty());
    CHECK(oneUnit.front() == 0x7fff);

    // Half volume scales every sample linearly.
    auto const half = synthesizeNote(3.5, 1, 880.0);
    REQUIRE(half.size() == oneUnit.size());
    CHECK(half.front() == static_cast<int16_t>(0x7fff / 2));

    // The wave actually alternates: some sample must be negative at a nonzero frequency.
    CHECK(std::ranges::any_of(oneUnit, [](auto s) { return s < 0; }));

    // Frequency 0 (note 0) never goes negative — constant level, i.e. silence to the ear.
    auto const silent = createMusicalNote(7.0, 1, 0);
    CHECK(std::ranges::none_of(silent, [](auto s) { return s < 0; }));

    // Longer duration scales the sample count.
    auto const twoUnits = synthesizeNote(7.0, 2, 880.0);
    CHECK(twoUnits.size() == static_cast<size_t>(std::ceil(2.0 / 32.0 * SampleRate)));
}

TEST_CASE("audio: renderNotesToPcm concatenates notes as 16-bit PCM bytes", "[audio]")
{
    using contour::audio::createMusicalNote;
    using contour::audio::renderNotesToPcm;

    // Two notes render to the concatenation of their per-note sample bytes (2 bytes/sample).
    auto const notes = std::array<int, 2> { 10, 12 };
    auto const bytes = renderNotesToPcm(7, 1, std::span<int const>(notes.data(), notes.size()));

    auto const n0 = createMusicalNote(7, 1, 10);
    auto const n1 = createMusicalNote(7, 1, 12);
    CHECK(bytes.size() == (n0.size() + n1.size()) * sizeof(std::int16_t));

    // An empty note sequence yields no bytes.
    CHECK(renderNotesToPcm(7, 1, std::span<int const> {}).empty());
}
