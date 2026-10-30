// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/CaptureLineMode.hpp>
#include <vtbackend/core/Primitives.hpp>

#include <chrono>
#include <expected>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>

namespace contour::cli
{

struct CaptureSettings
{
    vtbackend::CaptureLineMode lineMode = vtbackend::CaptureLineMode::Physical; // -l
    bool words = false;     // split output into one word per line
    double timeout = 1.0f;  // -t <timeout in seconds>
    std::string outputFile; // -o <outputfile>
    int verbosityLevel = 0; // -v, -q (XXX intentionally not parsed currently!)
    vtbackend::LineCount lineCount = vtbackend::LineCount { 0 }; // (use terminal default)
};

/// The byte transport a capture session talks through: capture requests are written to the peer
/// terminal, replies are read back under a timeout. Production talks to the controlling TTY (raw
/// mode); tests inject a canned VT peer, making the capture protocol machine-checkable.
class CaptureTransport
{
  public:
    virtual ~CaptureTransport() = default;

    /// Waits for readable data, for at most @p timeout, which it decreases by the time it waited, as
    /// select(2) does.
    /// @param timeout The time left, in and out.
    /// @return >0 when readable, 0 on timeout, <0 on error.
    virtual int wait(std::chrono::microseconds& timeout) = 0;

    /// Reads at most @p size bytes into @p buf. @return bytes read, or <0 on error.
    virtual int read(void* buf, std::size_t size) = 0;

    /// Writes @p chunk to the peer terminal. @return bytes written, or <0 on error.
    virtual int write(std::string_view chunk) = 0;
};

#ifdef _WIN32
/// The input mode of a Windows console, which a capture switches to raw input.
class ConsoleInput
{
  public:
    virtual ~ConsoleInput() = default;

    /// @return The console's input mode (GetConsoleMode()); std::nullopt where it has none to read.
    [[nodiscard]] virtual std::optional<unsigned long> mode() const = 0;

    /// Sets the console's input mode (SetConsoleMode()).
    /// @param mode The mode to set.
    /// @return Nothing once the console took it; else the error it gave (GetLastError()).
    [[nodiscard]] virtual std::expected<void, unsigned long> setMode(unsigned long mode) = 0;
};

/// Switches @p console to what a capture reads: virtual-terminal input, neither line editing nor echo.
/// @return The mode to put back afterwards, once the console took the new one; std::nullopt where it did
///         not, and a capture cannot read its replies.
[[nodiscard]] std::optional<unsigned long> enterCaptureInputMode(ConsoleInput& console);

/// Puts @p console back into the input mode a capture found it in, and tells @p diagnostics, with the
/// console's error, when it refuses: the console is then left without line editing or echo, which the user
/// has to know.
/// @param console The console.
/// @param saved The mode enterCaptureInputMode() returned.
/// @param diagnostics Where a refusal is reported.
void leaveCaptureInputMode(ConsoleInput& console, unsigned long saved, std::ostream& diagnostics);
#endif

/// Runs one capture session over @p transport, writing the captured lines to @p output.
/// @return true when the peer delivered a complete capture; false on timeout/protocol/IO failure.
bool captureScreen(CaptureSettings const& settings, CaptureTransport& transport, std::ostream& output);

/// Production entry point: talks to the controlling TTY and resolves settings.outputFile ("-" =
/// stdout) before delegating to the transport-based overload above.
bool captureScreen(CaptureSettings const& settings);

} // namespace contour::cli
