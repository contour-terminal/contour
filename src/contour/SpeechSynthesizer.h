// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>
#include <string>
#include <string_view>

namespace contour
{

/// Prepares @p text for being spoken aloud.
///
/// Terminal text is laid out for the eye: lines are padded to the grid, and a command's output is
/// usually separated from the next by blank rows. Read verbatim that becomes long silences and a
/// stream of trailing spaces, so the padding goes and a run of blank lines collapses to one.
///
/// @param text     The selected text, lines separated by '\n'.
/// @param maxChars Upper bound on the result; a selection longer than this is cut at a line boundary
///                 where one is near, so a whole build log cannot become minutes of speech.
/// @return The text to speak, empty when there is nothing worth saying.
[[nodiscard]] std::string speakableText(std::string_view text, size_t maxChars);

/// Speaks text aloud through the operating system.
///
/// An interface because AGENT.md asks for one around anything ambient, and because the module behind
/// it is OPTIONAL: a build without Qt's TextToSpeech still has to compile, still has to run, and still
/// has to make sensible decisions about whether to offer the feature at all.
class SpeechSynthesizer
{
  public:
    SpeechSynthesizer() = default;
    SpeechSynthesizer(SpeechSynthesizer const&) = delete;
    SpeechSynthesizer& operator=(SpeechSynthesizer const&) = delete;
    SpeechSynthesizer(SpeechSynthesizer&&) = delete;
    SpeechSynthesizer& operator=(SpeechSynthesizer&&) = delete;
    virtual ~SpeechSynthesizer() = default;

    /// Whether speaking is possible at all.
    ///
    /// False when the module was not built in, and false when it was but the platform offers no voice.
    /// The menu row asks this rather than offering something that would silently do nothing.
    [[nodiscard]] virtual bool available() const = 0;

    /// Speaks @p text, interrupting whatever is being spoken.
    virtual void say(std::string_view text) = 0;

    /// Stops speaking.
    virtual void stop() = 0;
};

/// A SpeechSynthesizer that cannot speak.
///
/// Used when Qt's TextToSpeech module is not part of the build. Reports unavailable, so the feature
/// disappears from the menu rather than appearing and doing nothing.
class NullSpeechSynthesizer final: public SpeechSynthesizer
{
  public:
    [[nodiscard]] bool available() const override { return false; }
    void say(std::string_view /*text*/) override {}
    void stop() override {}
};

/// The synthesizer this build can offer.
///
/// Returns a NullSpeechSynthesizer when Qt's TextToSpeech module was not available at configure time;
/// see CONTOUR_WITH_TTS in src/contour/CMakeLists.txt, which also makes the omission visible in the
/// build log rather than leaving it to be discovered from a bug report.
[[nodiscard]] std::unique_ptr<SpeechSynthesizer> makeSpeechSynthesizer();

} // namespace contour
