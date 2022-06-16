#pragma once

#include <crispy/span.h>

#include <memory>

#include <qbuffer.h>
#include <qthread.h>

#if QT_VERSION >= 0x060000
    #include <QtMultimedia/QAudioSink>
#else
    #include <QtMultimedia/QAudioOutput>
#endif
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
    void fillBuffer(int volume, int duration, crispy::span<const int> notes);
    std::vector<std::int16_t> createMusicalNote(double volume, int duration, int note_) noexcept;

    QByteArray byteArray_;
    QBuffer audioBuffer_;
    QThread soundThread_;
#if QT_VERSION >= 0x060000
    std::unique_ptr<QAudioSink> audio;
#else
    std::unique_ptr<QAudioOutput> audio;
#endif
};
} // namespace contour
