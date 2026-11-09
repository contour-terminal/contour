// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

class QObject;
class QThread;

namespace contour::platform
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

/// Runs another SpeechSynthesizer -- the engine -- on a thread of its own, so nothing it does can stall
/// the caller's.
///
/// Bringing a platform engine up is slow: Qt's speech-dispatcher backend enumerates every installed
/// voice, over a second with espeak-ng's thousands of them, and asking an engine whether it can speak
/// at all means bringing it up. The menus ask that every time they open, on the GUI thread. So the
/// engine is built, asked once, and from then on driven on a dedicated QThread -- one with an event
/// loop, because QTextToSpeech has thread affinity and its backends may rely on that loop -- and the
/// caller only ever reads the published answer or queues work.
///
/// The probe is queued in the constructor, i.e. at application startup, so the answer is usually in
/// before anyone opens a menu, and the cost is paid on a thread nobody waits for. Until it is in,
/// available() says no, which hides the speech rows rather than offering something that may not work.
///
/// Qt delivers queued calls only while a QCoreApplication exists: construct this once the application
/// object exists, and expect what is still queued when it goes to be dropped.
class ThreadedSpeechSynthesizer final: public SpeechSynthesizer
{
  public:
    /// Builds the engine. Runs on the worker thread, which then owns the engine for its whole life.
    /// May return null, which reads as "no speech here".
    using EngineFactory = std::function<std::unique_ptr<SpeechSynthesizer>()>;

    /// Starts the worker thread and queues the engine's construction and probe on it; returns at once.
    /// @param factory Builds the engine on the worker thread; must not be empty.
    explicit ThreadedSpeechSynthesizer(EngineFactory factory);

    /// Ends the worker's loop, destroys the engine on the worker as the thread finishes, and joins it.
    /// Calls still queued are dropped. Waits for a factory or an engine call already in progress; there
    /// is no safe way to abandon one.
    ~ThreadedSpeechSynthesizer() override;

    ThreadedSpeechSynthesizer(ThreadedSpeechSynthesizer const&) = delete;
    ThreadedSpeechSynthesizer& operator=(ThreadedSpeechSynthesizer const&) = delete;
    ThreadedSpeechSynthesizer(ThreadedSpeechSynthesizer&&) = delete;
    ThreadedSpeechSynthesizer& operator=(ThreadedSpeechSynthesizer&&) = delete;

    /// The engine's answer, read without waiting; false while the engine is still being probed.
    [[nodiscard]] bool available() const override;

    /// Queues @p text for the engine, behind anything queued earlier (its construction included).
    void say(std::string_view text) override;

    /// Queues a stop for the engine; harmless when nothing was ever spoken, or there is no engine.
    void stop() override;

  private:
    /// The worker thread, with a Qt event loop.
    std::unique_ptr<QThread> _thread;
    /// Lives on _thread; the receiver every queued call is posted to.
    std::unique_ptr<QObject> _receiver;
    /// Created, used and destroyed on _thread only.
    std::unique_ptr<SpeechSynthesizer> _engine;
    /// Written once by the worker after the probe, read by available() from any thread.
    std::atomic<bool> _isAvailable { false };
};

/// The synthesizer this build can offer.
///
/// With Qt's TextToSpeech module, a ThreadedSpeechSynthesizer around a QTextToSpeech engine, whose
/// probe therefore begins as soon as this returns, off the calling thread. Call it once a
/// QCoreApplication exists (@see ThreadedSpeechSynthesizer).
/// Returns a NullSpeechSynthesizer when Qt's TextToSpeech module was not available at configure time;
/// see CONTOUR_WITH_TTS in src/contour/CMakeLists.txt, which also makes the omission visible in the
/// build log rather than leaving it to be discovered from a bug report.
[[nodiscard]] std::unique_ptr<SpeechSynthesizer> makeSpeechSynthesizer();

} // namespace contour::platform
