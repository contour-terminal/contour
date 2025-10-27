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
    void fillBuffer(int volume, int duration, gsl::span<const int> notes);
    static std::vector<std::int16_t> createMusicalNote(double volume, int duration, int note) noexcept;

    QByteArray _byteArray;
    QBuffer _audioBuffer;
    QThread _soundThread;
    std::unique_ptr<QAudioSink> _audioSink;
};
} // namespace contour
