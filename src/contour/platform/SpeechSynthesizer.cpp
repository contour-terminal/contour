// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/SpeechSynthesizer.h>

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

#ifdef CONTOUR_WITH_TTS

namespace
{
    /// Speaks through Qt's TextToSpeech module.
    ///
    /// The engine is built on first use rather than with this object. Constructing a QTextToSpeech
    /// loads the platform's speech plugin and opens a connection to its service — speech-dispatcher on
    /// Linux — to enumerate the installed voices, and that connection leaks its voice-list reply
    /// inside libspeechd. A contour nobody ever asks to read anything aloud should pay none of that,
    /// so holding this object stays free and the engine appears only when something is actually said
    /// or asked about.
    class QtSpeechSynthesizer final: public SpeechSynthesizer
    {
      public:
        [[nodiscard]] bool available() const override
        {
            // Built in, but a platform with no engine or no installed voice still cannot speak; on Linux
            // that is a machine without speech-dispatcher or flite. Asked rather than assumed, so the
            // menu row does not offer silence.
            auto const& speech = engine();
            return speech.state() != QTextToSpeech::Error && !speech.availableVoices().isEmpty();
        }

        void say(std::string_view text) override
        {
            // say() already replaces what is being spoken; stopping first makes that explicit and is
            // what makes a second invocation feel like "read THIS" rather than "queue this up".
            auto& speech = engine();
            speech.stop();
            speech.say(QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())));
        }

        void stop() override
        {
            // Deliberately does NOT build the engine: nothing has been spoken through one that does not
            // exist, so there is nothing to stop, and constructing it here to stop it immediately would
            // undo the deferral this class exists for.
            if (_speech)
                _speech->stop();
        }

      private:
        /// The engine, constructed on the first call. @see the class description for why it waits.
        /// @return The engine, which from here on outlives every use of it.
        [[nodiscard]] QTextToSpeech& engine() const
        {
            if (!_speech)
                _speech = std::make_unique<QTextToSpeech>();
            return *_speech;
        }

        /// Mutable because asking whether speech is possible is a const question whose answer only the
        /// engine knows. Deferring construction is an implementation detail; it must not force the call
        /// sites into a non-const path, nor into sequencing a setup call of their own.
        mutable std::unique_ptr<QTextToSpeech> _speech;
    };
} // namespace

std::unique_ptr<SpeechSynthesizer> makeSpeechSynthesizer()
{
    return std::make_unique<QtSpeechSynthesizer>();
}

#else

std::unique_ptr<SpeechSynthesizer> makeSpeechSynthesizer()
{
    return std::make_unique<NullSpeechSynthesizer>();
}

#endif

} // namespace contour::platform
