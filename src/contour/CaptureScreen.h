// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/primitives.h>

#include <iosfwd>
#include <string>
#include <string_view>

struct timeval;

namespace contour
{

struct CaptureSettings
{
    bool logicalLines = false; // -l
    bool words = false;        // split output into one word per line
    double timeout = 1.0f;     // -t <timeout in seconds>
    std::string outputFile;    // -o <outputfile>
    int verbosityLevel = 0;    // -v, -q (XXX intentionally not parsed currently!)
    vtbackend::LineCount lineCount = vtbackend::LineCount { 0 }; // (use terminal default)
};

/// The byte transport a capture session talks through: capture requests are written to the peer
/// terminal, replies are read back under a timeout. Production talks to the controlling TTY (raw
/// mode); tests inject a canned VT peer, making the capture protocol machine-checkable.
class CaptureTransport
{
  public:
    virtual ~CaptureTransport() = default;

    /// Waits for readable data. @param timeout In/out remaining time, select(2) semantics.
    /// @return >0 when readable, 0 on timeout, <0 on error.
    virtual int wait(timeval* timeout) = 0;

    /// Reads at most @p size bytes into @p buf. @return bytes read, or <0 on error.
    virtual int read(void* buf, std::size_t size) = 0;

    /// Writes @p chunk to the peer terminal. @return bytes written, or <0 on error.
    virtual int write(std::string_view chunk) = 0;
};

/// Runs one capture session over @p transport, writing the captured lines to @p output.
/// @return true when the peer delivered a complete capture; false on timeout/protocol/IO failure.
bool captureScreen(CaptureSettings const& settings, CaptureTransport& transport, std::ostream& output);

/// Production entry point: talks to the controlling TTY and resolves settings.outputFile ("-" =
/// stdout) before delegating to the transport-based overload above.
bool captureScreen(CaptureSettings const& settings);

} // namespace contour
