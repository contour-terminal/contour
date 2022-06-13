#include <contour/Audio.h>

#include <crispy/assert.h>
#include <crispy/logstore.h>

#include <qbuffer.h>
#include <qthread.h>

#if QT_VERSION >= 0x060000
    #include <QtMultimedia/QMediaDevices>
#endif

using namespace contour;

namespace
{
constexpr double SAMPLE_RATE = 44100;

// TODO make this function constexpr when we switch to c++23
double square_wave(double x) noexcept
{
    x = std::fmod(x, 2);
    return std::isgreater(x, 1) ? -1 : 1;
}

auto createMusicalNote(double volume, double duration, double frequency) noexcept
{
    duration = duration / 32.0;
    volume /= 7;
    std::vector<std::int16_t> buffer;
    buffer.reserve(static_cast<size_t>(std::ceil(duration * SAMPLE_RATE)));
    for (double i = 0; i < duration * SAMPLE_RATE; ++i)
        buffer.push_back(
            static_cast<int16_t>(0x7fff * volume * square_wave(frequency / SAMPLE_RATE * i * 2)));
    return buffer;
}

} // namespace

Audio::Audio()
{
    QAudioFormat f;
    f.setSampleRate(44100);
    f.setChannelCount(1);

#if QT_VERSION >= 0x060000
    f.setSampleFormat(QAudioFormat::Int16);
    QAudioDevice info(QMediaDevices::defaultAudioOutput());
#else
    f.setSampleSize(16);
    f.setCodec("audio/pcm");
    f.setByteOrder(QAudioFormat::LittleEndian);
    f.setSampleType(QAudioFormat::SignedInt);
    QAudioDeviceInfo info(QAudioDeviceInfo::defaultOutputDevice());
#endif

    if (!info.isFormatSupported(f))
    {
        errorlog()("Default output device doesn't support 16 Bit signed integer PCM");
        return;
    }

#if QT_VERSION < 0x060000
    using QAudioSink = QAudioOutput;
#endif

    audio = std::make_unique<QAudioSink>(f);

    audio->moveToThread(&soundThread_);

    connect(audio.get(), &QAudioSink::stateChanged, this, &Audio::handleStateChanged);
    qRegisterMetaType<std::vector<int>>();
    connect(this, &Audio::play, this, &Audio::handlePlayback);
    soundThread_.start();
}

Audio::~Audio()
{
    soundThread_.quit();
    soundThread_.wait();
}

void Audio::fillBuffer(int volume, int duration, crispy::span<int const> notes)
{
    for (auto const i: notes)
    {
        auto b = createMusicalNote(volume, duration, i);
        byteArray_.append(reinterpret_cast<char const*>(b.data()), static_cast<int>(2 * b.size()));
    }
}

void Audio::handlePlayback(int volume, int duration, std::vector<int> const& notes)
{
    Require(audio);
    if (audio->state() == QAudio::State::ActiveState)
    {
        fillBuffer(volume, duration, crispy::span(notes.data(), notes.size()));
        return;
    }
    fillBuffer(volume, duration, crispy::span(notes.data(), notes.size()));
    audioBuffer_.setBuffer(&byteArray_);
    audioBuffer_.open(QIODevice::ReadWrite);
    audio->start(&audioBuffer_);
}

void Audio::handleStateChanged(QAudio::State state)
{
    switch (state)
    {
        case QAudio::IdleState:
            audio->stop();
            audioBuffer_.close();
            byteArray_.clear();
            break;

        case QAudio::StoppedState:
            if (audio->error() != QAudio::NoError)
            {
                errorlog()("Audio playback stopped: {}", audio->error());
            }
            break;
        default: break;
    }
}

std::vector<std::int16_t> Audio::createMusicalNote(double volume, int duration, int note_) noexcept
{
    Require(static_cast<int>(note_) >= 0 && static_cast<int>(note_) < 26);
    double frequency = note_ == 0 ? 0 : 440.0 * std::pow(2, (note_ + 2) / 12.0);
    return ::createMusicalNote(volume, duration, frequency);
}
