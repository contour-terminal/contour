// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/QtInvoke.hpp>
#include <contour/platform/SpeechSynthesizer.hpp>

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QThread>

#ifdef CONTOUR_WITH_TTS
    #include <QtTextToSpeech/QTextToSpeech>
#endif

#include <algorithm>
#include <vector>

namespace contour::platform
{

std::string speakableText(std::string_view text, size_t maxChars)
{
    auto lines = std::vector<std::string> {};
    auto start = size_t { 0 };
    while (start <= text.size())
    {
        auto const end = text.find('\n', start);
        auto line = std::string { text.substr(start, end == std::string_view::npos ? end : end - start) };

        // Grid padding: every cell of a terminal line exists whether or not anything was written to it,
        // so a selection carries the blanks out to the right margin. Spoken, those are just silence.
        while (!line.empty() && (line.back() == ' ' || line.back() == '\r' || line.back() == '\t'))
            line.pop_back();
        lines.push_back(std::move(line));

        if (end == std::string_view::npos)
            break;
        start = end + 1;
    }

    // Blank lines separate output visually and say nothing aloud, so a run of them becomes one pause.
    auto collapsed = std::string {};
    auto previousWasBlank = false;
    for (auto const& line: lines)
    {
        auto const blank = line.empty();
        if (blank && previousWasBlank)
            continue;
        previousWasBlank = blank;

        if (!collapsed.empty())
            collapsed += '\n';
        collapsed += line;
    }

    // Leading and trailing blank lines are pure padding.
    while (!collapsed.empty() && collapsed.front() == '\n')
        collapsed.erase(collapsed.begin());
    while (!collapsed.empty() && collapsed.back() == '\n')
        collapsed.pop_back();

    if (collapsed.size() <= maxChars)
        return collapsed;

    // Cut at a line boundary when one is reasonably near the limit, so speech stops at something that
    // sounds finished rather than mid-word.
    auto cut = collapsed.substr(0, maxChars);
    if (auto const lastNewline = cut.rfind('\n');
        lastNewline != std::string::npos && lastNewline > maxChars / 2)
        cut.erase(lastNewline);
    return cut;
}

ThreadedSpeechSynthesizer::ThreadedSpeechSynthesizer(EngineFactory factory):
    _thread { std::make_unique<QThread>() }, _receiver { std::make_unique<QObject>() }
{
    _thread->setObjectName(QStringLiteral("speech"));
    _receiver->moveToThread(_thread.get());

    // The engine goes on the thread it was built on, once that thread's loop has ended. Driven by the
    // thread finishing rather than by a call queued to it, because an owner may outlive its application,
    // after which queued calls are never delivered.
    QObject::connect(
        _thread.get(), &QThread::finished, _thread.get(), [this] { _engine.reset(); }, Qt::DirectConnection);
    _thread->start();

    // The first thing the worker does, so everything queued after it finds the engine already built.
    postToObject(_receiver.get(), [this, factory = std::move(factory)] {
        _engine = factory();
        if (_engine && _engine->available())
            _isAvailable.store(true, std::memory_order_release);
        else
            _engine.reset(); // it cannot speak: release its plugin and service connection
    });
}

ThreadedSpeechSynthesizer::~ThreadedSpeechSynthesizer()
{
    // Not queued: with no QCoreApplication left a queued call would never run, and this would wait for
    // it forever. A call in progress (the probe included) finishes first; what is still queued is
    // dropped, which at shutdown is what stopping the speech would have done anyway.
    _thread->quit();
    _thread->wait();
    // _receiver's thread has finished, so it may now be destroyed from here.
}

bool ThreadedSpeechSynthesizer::available() const
{
    return _isAvailable.load(std::memory_order_acquire);
}

void ThreadedSpeechSynthesizer::say(std::string_view text)
{
    postToObject(_receiver.get(), [this, text = std::string { text }] {
        if (_engine)
            _engine->say(text);
    });
}

void ThreadedSpeechSynthesizer::stop()
{
    postToObject(_receiver.get(), [this] {
        if (_engine)
            _engine->stop();
    });
}

#ifdef CONTOUR_WITH_TTS

namespace
{
    /// Speaks through Qt's TextToSpeech module.
    ///
    /// Constructing it loads the platform's speech plugin and connects to its service --
    /// speech-dispatcher on Linux -- which enumerates the installed voices; with espeak-ng that is
    /// thousands of them and takes a second or more. It is therefore only ever built, used and
    /// destroyed on a ThreadedSpeechSynthesizer's worker, and built as soon as the application starts: an
    /// early answer paid for off the GUI thread is worth more than the connection it costs.
    class QtSpeechSynthesizer final: public SpeechSynthesizer
    {
      public:
        [[nodiscard]] bool available() const override
        {
            // Built in, but a platform with no engine or no installed voice still cannot speak; on Linux
            // that is a machine without speech-dispatcher or flite. Asked rather than assumed, so the
            // menu row does not offer silence.
            return _speech.state() != QTextToSpeech::Error && !_speech.availableVoices().isEmpty();
        }

        void say(std::string_view text) override
        {
            // say() already replaces what is being spoken; stopping first makes that explicit and is
            // what makes a second invocation feel like "read THIS" rather than "queue this up".
            _speech.stop();
            _speech.say(QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())));
        }

        void stop() override { _speech.stop(); }

      private:
        QTextToSpeech _speech;
    };
} // namespace

std::unique_ptr<SpeechSynthesizer> makeSpeechSynthesizer()
{
    return std::make_unique<ThreadedSpeechSynthesizer>(
        []() -> std::unique_ptr<SpeechSynthesizer> { return std::make_unique<QtSpeechSynthesizer>(); });
}

#else

std::unique_ptr<SpeechSynthesizer> makeSpeechSynthesizer()
{
    return std::make_unique<NullSpeechSynthesizer>();
}

#endif

} // namespace contour::platform
