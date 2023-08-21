#pragma once

#include <gsl/span>

#include <memory>

#include <qbuffer.h>
#include <qthread.h>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    #include <QtMultimedia/QAudioSink>
#else
    #include <QtMultimedia/QAudioOutput>
#endif
namespace contour
{

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
using QtAudioSink = QAudioSink;
#else
using QtAudioSink = QAudioOutput;
#endif

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
    std::unique_ptr<QtAudioSink> _audioSink;
};
} // namespace contour
