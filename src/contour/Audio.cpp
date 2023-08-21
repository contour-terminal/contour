#include <contour/Audio.h>

#include <crispy/assert.h>
#include <crispy/logstore.h>

#include <qbuffer.h>
#include <qthread.h>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    #include <QtMultimedia/QMediaDevices>
#endif

using namespace contour;

namespace
{
constexpr double SampleRate = 44100;

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
    buffer.reserve(static_cast<size_t>(std::ceil(duration * SampleRate)));
    for (double i = 0; i < duration * SampleRate; ++i)
        buffer.push_back(static_cast<int16_t>(0x7fff * volume * square_wave(frequency / SampleRate * i * 2)));
    return buffer;
}

} // namespace

Audio::Audio()
{
    QAudioFormat f;
    f.setSampleRate(44100);
    f.setChannelCount(1);

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
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
        errorLog()("Default output device doesn't support 16 Bit signed integer PCM");
        return;
    }

    _audioSink = std::make_unique<QtAudioSink>(f);

    _audioSink->moveToThread(&_soundThread);

    connect(_audioSink.get(), &QtAudioSink::stateChanged, this, &Audio::handleStateChanged);
    qRegisterMetaType<std::vector<int>>();
    connect(this, &Audio::play, this, &Audio::handlePlayback);
    _soundThread.start();
}

Audio::~Audio()
{
    _soundThread.quit();
    _soundThread.wait();
}

void Audio::fillBuffer(int volume, int duration, gsl::span<int const> notes)
{
    for (auto const i: notes)
    {
        auto b = createMusicalNote(volume, duration, i);
        _byteArray.append(reinterpret_cast<char const*>(b.data()), static_cast<int>(2 * b.size()));
    }
}

void Audio::handlePlayback(int volume, int duration, std::vector<int> const& notes)
{
    Require(_audioSink);
    if (_audioSink->state() == QAudio::State::ActiveState)
    {
        fillBuffer(volume, duration, gsl::span(notes.data(), notes.size()));
        return;
    }
    fillBuffer(volume, duration, gsl::span(notes.data(), notes.size()));
    _audioBuffer.setBuffer(&_byteArray);
    _audioBuffer.open(QIODevice::ReadWrite);
    _audioSink->start(&_audioBuffer);
}

void Audio::handleStateChanged(QAudio::State state)
{
    switch (state)
    {
        case QAudio::IdleState:
            _audioSink->stop();
            _audioBuffer.close();
            _byteArray.clear();
            break;

        case QAudio::StoppedState:
            if (_audioSink->error() != QAudio::NoError)
            {
                errorLog()("Audio playback stopped: {}", static_cast<int>(_audioSink->error()));
            }
            break;
        default: break;
    }
}

std::vector<std::int16_t> Audio::createMusicalNote(double volume, int duration, int note) noexcept
{
    Require(static_cast<int>(note) >= 0 && static_cast<int>(note) < 26);
    double frequency = note == 0 ? 0 : 440.0 * std::pow(2, (note + 2) / 12.0);
    return ::createMusicalNote(volume, duration, frequency);
}
