// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cassert>
#include <cmath>
#include <cstdint>
#include <ranges>
#include <span>
#include <vector>

// Pure PCM synthesis for the terminal bell (DECPS): dependency-free (no Qt, no audio device) so the
// waveform math is unit-testable — the established extraction pattern (ScissorRect, WindowGeometry,
// ScreenshotReadback). contour::Audio interprets these samples through QAudioSink.
namespace contour::audio
{

/// Samples per second of the synthesized PCM stream (matches the QAudioSink format in Audio.cpp).
constexpr double SampleRate = 44100.0;

// On Windows, this cannot be constexpr just yet, because std::fmod is not constexpr in MSVC (yet).
#ifndef _WIN32
    #define CONTOUR_CONSTEXPR_UNLESS_MSVC constexpr
#else
    #define CONTOUR_CONSTEXPR_UNLESS_MSVC
#endif

/// Square wave with period 2 over @p x: +1 on [0,1], -1 on (1,2).
CONTOUR_CONSTEXPR_UNLESS_MSVC inline double squareWave(double x) noexcept
{
    x = std::fmod(x, 2);
    return std::isgreater(x, 1) ? -1 : 1;
}

/// Maps a DECPS note number to its frequency in Hz.
/// @param note Note number in [0, 26); 0 means silence (0 Hz).
/// @return The equal-tempered frequency, anchored so note 10 is 880 Hz (A5).
[[nodiscard]] inline double noteToFrequency(int note) noexcept
{
    assert(note >= 0 && note < 26);
    return note == 0 ? 0.0 : 440.0 * std::pow(2, (note + 2) / 12.0);
}

/// Synthesizes one square-wave note as signed 16-bit PCM samples.
/// @param volume    Linear volume in [0, 7] (DECPS volume range); scaled internally by 1/7.
/// @param duration  Note duration in 1/32s units (DECPS duration).
/// @param frequency Tone frequency in Hz; 0 yields a constant (silent-to-the-ear) level.
/// @return ceil(duration/32 * SampleRate) samples.
[[nodiscard]] inline std::vector<std::int16_t> synthesizeNote(double volume,
                                                              double duration,
                                                              double frequency)
{
    duration = duration / 32.0;
    volume /= 7;
    auto const sampleCount = static_cast<int>(std::ceil(duration * SampleRate));
    std::vector<std::int16_t> buffer;
    buffer.reserve(static_cast<size_t>(sampleCount));
    for (auto const i: std::views::iota(0, sampleCount))
        buffer.push_back(static_cast<int16_t>(0x7fff * volume * squareWave(frequency / SampleRate * i * 2)));
    return buffer;
}

/// Synthesizes a DECPS note (see noteToFrequency + synthesizeNote).
[[nodiscard]] inline std::vector<std::int16_t> createMusicalNote(double volume, int duration, int note)
{
    return synthesizeNote(volume, duration, noteToFrequency(note));
}

/// Renders a DECPS note sequence into a contiguous signed-16-bit little-endian PCM byte stream —
/// the exact bytes contour::Audio hands to QAudioSink. Kept dependency-free (no Qt) so the
/// note-sequence assembly is unit-testable without an audio device.
/// @param volume   Linear volume in [0, 7].
/// @param duration Per-note duration in 1/32s units.
/// @param notes    Note numbers, each in [0, 26).
/// @return The concatenated notes as raw PCM bytes (2 bytes/sample, host byte order).
[[nodiscard]] inline std::vector<char> renderNotesToPcm(int volume, int duration, std::span<int const> notes)
{
    std::vector<char> bytes;
    for (auto const note: notes)
    {
        assert(note >= 0 && note < 26);
        auto const samples = createMusicalNote(volume, duration, note);
        auto const* const raw = reinterpret_cast<char const*>(samples.data());
        bytes.insert(bytes.end(), raw, raw + (samples.size() * sizeof(std::int16_t)));
    }
    return bytes;
}

} // namespace contour::audio
