// SPDX-License-Identifier: Apache-2.0
#include <contour/SpeechSynthesizer.h>

#if defined(CONTOUR_WITH_TTS)
    #include <QtTextToSpeech/QTextToSpeech>
#endif

#include <algorithm>
#include <vector>

namespace contour
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

#if defined(CONTOUR_WITH_TTS)

namespace
{
    /// Speaks through Qt's TextToSpeech module.
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
    return std::make_unique<QtSpeechSynthesizer>();
}

#else

std::unique_ptr<SpeechSynthesizer> makeSpeechSynthesizer()
{
    return std::make_unique<NullSpeechSynthesizer>();
}

#endif

} // namespace contour
