#pragma once

#include <gsl/span>

#include <QtCore/QBuffer>
#include <QtCore/QThread>
#include <QtMultimedia/QAudioSink>

#include <memory>

namespace contour
{

class Audio: public QObject
{
    Q_OBJECT

  public:
    Audio();
    ~Audio() override;
  signals:
    void play(int volume, int duration, std::vector<int> const& notes);
  private slots:
    void handleStateChanged(QAudio::State state);
    void handlePlayback(int volume, int duration, std::vector<int> const& notes);

  private:
    /// Appends the PCM synthesis of @p notes (see contour::audio, AudioNote.h) to the playback buffer.
    void fillBuffer(int volume, int duration, gsl::span<int const> notes);

    QByteArray _byteArray;
    QBuffer _audioBuffer;
    QThread _soundThread;
    std::unique_ptr<QAudioSink> _audioSink;
};
} // namespace contour
