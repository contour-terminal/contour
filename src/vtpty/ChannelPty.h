// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `ChannelPty` — a process-less Pty fed and drained by application code.
///
/// Where MockPty reports EOF the moment its buffer drains (right for
/// synchronous parse-what-you-seeded tests), ChannelPty mirrors UnixPty's
/// blocking contract so a live session survives between feeds: an empty
/// buffer BLOCKS the reader (honoring the read timeout) until data arrives,
/// a wakeupReader(), or close(); EOF (an empty read) is reported only once
/// closed. This is the seam a remotely-populated terminal session runs on:
/// network code feed()s received bytes to the parser pump, and optional
/// sinks observe what the terminal writes (input) and requests (resize).

#include <vtpty/Pty.h>

#include <crispy/BufferObject.h>

#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace vtpty
{

/// A thread-safe, blocking-read Pty without a child process.
///
/// Deliberately NOT final, for two independent reasons: a consumer subclasses it to observe
/// destruction (unbinding a registry entry), and a test varies start() alone (contour's
/// ConfigurableStartPty) to drive the shell-start failure paths against a device that stays UP --
/// which is the whole reason to build on this rather than MockPty, whose empty-buffer read is
/// read as EOF.
class ChannelPty: public Pty
{
  public:
    /// Receives every chunk the terminal writes to its "stdin" (keyboard and
    /// mouse encodings, replies). Invoked on the writing thread, outside the
    /// pty's lock. When unset, writes accumulate in a buffer instead.
    using WriteSink = std::function<void(std::string_view)>;

    /// Receives every resize request the terminal issues. Invoked on the
    /// resizing thread, outside the pty's lock.
    using ResizeSink = std::function<void(PageSize, std::optional<ImageSize>)>;

    /// @param windowSize The initial page size reported by pageSize().
    explicit ChannelPty(PageSize windowSize): _pageSize { windowSize } {}

    /// Installs @p sink as the input receiver; buffered writes are unaffected.
    void setWriteSink(WriteSink sink);

    /// Installs @p sink as the resize-request receiver.
    void setResizeSink(ResizeSink sink);

    /// Feeds output to the (possibly blocked) reader, as a peer's bytes.
    void feed(std::string_view data);

    /// @return Everything the terminal wrote while no write sink was set.
    [[nodiscard]] std::string stdinSnapshot() const;

    /// @return Whether fed output is still waiting to be consumed.
    [[nodiscard]] bool isStdoutPending() const;

    /// @return How many bytes the output buffer holds right now, the already-read prefix included —
    ///         the device's resident output cost. Exposed so the bound feed() puts on it
    ///         (@ref CompactionThreshold) is a tested property rather than an assumed one.
    [[nodiscard]] std::size_t bufferedOutputBytes() const;

    /// How many resizes arrived after close(). A real PTY has no device left to resize by then --
    /// ConPty crashed outright on one (it handed the invalidated HPCON to ResizePseudoConsole) --
    /// so this must stay zero.
    [[nodiscard]] int resizesAfterClose() const noexcept;

    // vtpty::Pty overrides
    [[nodiscard]] PtySlave& slave() noexcept override { return _slave; }
    [[nodiscard]] std::optional<ReadResult> read(crispy::buffer_object<char>& storage,
                                                 std::optional<std::chrono::milliseconds> timeout,
                                                 size_t size) override;
    void wakeupReader() override;
    [[nodiscard]] int write(std::string_view data) override;
    [[nodiscard]] PageSize pageSize() const noexcept override;
    void resizeScreen(PageSize cells, std::optional<ImageSize> pixels = std::nullopt) override;
    /// Nothing to start: there is no child process. Reported as a plain success so the device
    /// satisfies the same contract a spawning pty does.
    [[nodiscard]] StartResult start() override { return StartOutcome {}; }
    void close() override;
    void waitForClosed() override;
    [[nodiscard]] bool isClosed() const noexcept override;

  private:
    /// How much consumed output prefix may accumulate before feed() reclaims it.
    ///
    /// A threshold rather than "compact every feed" so a burst of small chunks does not pay an
    /// O(n) shift each time, and rather than "compact only when fully drained" so a pane whose
    /// reader is never momentarily idle cannot grow the buffer without bound. Same value, and the
    /// same reasoning, as vthost::imsg::ImsgDecoder::feed and net::AsyncBufferedReader.
    static constexpr std::size_t CompactionThreshold = std::size_t { 64 } * 1024;

    mutable std::mutex _mutex;
    std::condition_variable _wakeup;
    PageSize _pageSize;
    std::optional<ImageSize> _pixelSize;
    WriteSink _writeSink;
    ResizeSink _resizeSink;
    std::string _inputBuffer;
    std::string _outputBuffer;
    std::size_t _outputReadOffset = 0;
    int _resizesAfterClose = 0; ///< Resizes that arrived once _closed; see resizesAfterClose().
    bool _closed = false;
    bool _woken = false; ///< One-shot wakeupReader() flag (teardown wake), cleared on read.
    PtySlaveDummy _slave;
};

} // namespace vtpty
