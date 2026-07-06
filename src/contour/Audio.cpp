
#include <contour/Audio.h>
#include <contour/AudioNote.h>

#include <crispy/assert.h>
#include <crispy/logstore.h>

#include <QtMultimedia/QMediaDevices>

using namespace contour;

Audio::Audio()
{
    QAudioFormat f;
    f.setSampleRate(44100);
    f.setChannelCount(1);

    f.setSampleFormat(QAudioFormat::Int16);
    QAudioDevice const info(QMediaDevices::defaultAudioOutput());

    if (!info.isFormatSupported(f))
    {
        errorLog()("Default output device doesn't support 16 Bit signed integer PCM");
        return;
    }

    _audioSink = std::make_unique<QAudioSink>(f);

    _audioSink->moveToThread(&_soundThread);

    connect(_audioSink.get(), &QAudioSink::stateChanged, this, &Audio::handleStateChanged);
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
    // The note→PCM assembly is pure (audio::renderNotesToPcm, unit-tested); this only appends the
    // bytes into the Qt buffer the sink reads from.
    auto const pcm =
        audio::renderNotesToPcm(volume, duration, std::span<int const>(notes.data(), notes.size()));
    _byteArray.append(pcm.data(), static_cast<int>(pcm.size()));
}

void Audio::handlePlayback(int volume, int duration, std::vector<int> const& notes)
{
    Require(_audioSink);

    // Append the new notes to the shared byte buffer regardless of the sink state.
    fillBuffer(volume, duration, gsl::span(notes.data(), notes.size()));

    // When the sink is already draining the buffer, the appended bytes are picked up in place — do
    // not re-open the buffer or restart the sink (that would rewind playback). Only prime and start
    // the sink when it is idle/stopped.
    if (_audioSink->state() == QAudio::State::ActiveState)
        return;

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
