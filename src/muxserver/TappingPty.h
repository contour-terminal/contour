// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `TappingPty` — a Pty decorator copying every read byte to a tap callback.
///
/// This is the daemon's BYTE tap: tmux control mode forwards the raw PTY output
/// stream (%output is byte-exact, the client emulates), so the bytes must be
/// captured BEFORE the terminal's parser consumes them. Decorating the Pty is
/// the injection seam that needs no vtbackend change: Terminal reads through
/// this wrapper on its pump thread, and the tap fires right there (the consumer
/// marshals onto its loop via EventLoop::post).

#include <vtpty/Pty.h>

#include <functional>
#include <memory>
#include <string_view>
#include <utility>

namespace muxserver
{

/// Forwards every Pty operation to the wrapped instance, copying read data to
/// the tap. The stdout-fast-pipe stream is NOT tapped: it is Contour's internal
/// side channel, not part of the application's output stream.
class TappingPty final: public vtpty::Pty
{
  public:
    /// Invoked on the READING thread with each consumed chunk.
    using Tap = std::function<void(std::string_view)>;

    /// @param inner The real PTY (owned).
    /// @param tap Receives every read chunk.
    TappingPty(std::unique_ptr<vtpty::Pty> inner, Tap tap): _inner(std::move(inner)), _tap(std::move(tap)) {}

    void start() override { _inner->start(); }
    [[nodiscard]] vtpty::PtySlave& slave() noexcept override { return _inner->slave(); }
    void close() override { _inner->close(); }
    void waitForClosed() override { _inner->waitForClosed(); }
    [[nodiscard]] bool isClosed() const noexcept override { return _inner->isClosed(); }

    [[nodiscard]] std::optional<ReadResult> read(crispy::buffer_object<char>& storage,
                                                 std::optional<std::chrono::milliseconds> timeout,
                                                 size_t size) override
    {
        auto result = _inner->read(storage, timeout, size);
        if (result && !result->fromStdoutFastPipe && !result->data.empty() && _tap)
            _tap(result->data);
        return result;
    }

    void wakeupReader() override { _inner->wakeupReader(); }
    [[nodiscard]] int write(std::string_view buf) override { return _inner->write(buf); }
    [[nodiscard]] vtpty::PageSize pageSize() const noexcept override { return _inner->pageSize(); }

    void resizeScreen(vtpty::PageSize cells, std::optional<vtpty::ImageSize> pixels = std::nullopt) override
    {
        _inner->resizeScreen(cells, pixels);
    }

    /// @return The wrapped PTY (for diagnostics/tests reaching the concrete type).
    [[nodiscard]] vtpty::Pty& inner() noexcept { return *_inner; }

  private:
    std::unique_ptr<vtpty::Pty> _inner;
    Tap _tap;
};

} // namespace muxserver
