// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/tui/Rect.hpp>
#include <core/tui/TerminalOutput.hpp>

#include <cstdint>
#include <functional>
#include <string_view>

namespace core::tui
{

/// @brief Receives the bytes an output flushes, in order.
using ByteSink = std::function<void(std::string_view bytes)>;

/// @brief Whether the bytes a @c ByteSinkOutput hands on are meant for a terminal.
enum class SinkDestination : std::uint8_t
{
    Plain,    ///< Not a terminal: a log, a file, a capture. No synchronized-output bracket.
    Terminal, ///< A terminal, somewhere past the sink: frames are bracketed in DEC mode 2026.
};

/// @brief A @c TerminalOutput whose destination is a @c ByteSink, and which touches no operating system.
///
/// Every member that would reach the process's standard output on its own -- the write, the size, the
/// capability probe, the terminal test -- is answered here instead, so this output behaves the same on
/// every platform and in a WebAssembly guest: its size is the one it was given, nothing is probed, and
/// @c isTerminal() says what the sink was declared to be.
class ByteSinkOutput final: public TerminalOutput
{
  public:
    /// @brief Constructs an output that hands every flushed byte to @p sink.
    /// @param sink Receives the bytes; must not be empty.
    /// @param size The size it reports, in cells.
    /// @param destination What the bytes are for.
    ByteSinkOutput(ByteSink sink, Size size, SinkDestination destination) noexcept;

    /// @brief Probes nothing.
    /// @return Success.
    [[nodiscard]] auto initialize() -> VoidResult override;
    /// @brief Probes nothing: unscroll support stays off.
    void detectCapabilities() override;
    /// @return Whether the sink was declared to be a terminal.
    [[nodiscard]] bool isTerminal() const noexcept override;
    /// @return The width given at construction or by @c resize().
    [[nodiscard]] auto columns() const noexcept -> int override;
    /// @return The height given at construction or by @c resize().
    [[nodiscard]] auto rows() const noexcept -> int override;
    /// @brief Measures nothing: the size is the one it was given.
    void updateDimensions() override;

    /// @brief Changes the size this output reports.
    ///
    /// Externally-driven geometry: the guest's host decides the size, not this output.
    ///
    /// @param size The new size, in cells.
    void resize(Size size) noexcept;

  protected:
    /// @brief Hands @p bytes to the sink.
    void writeToDestination(std::string_view bytes) override;

  private:
    ByteSink _sink;
    Size _size;
    SinkDestination _destination;
};

} // namespace core::tui
