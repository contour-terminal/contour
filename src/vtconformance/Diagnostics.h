// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace logstore
{
class sink;
}

namespace vtconformance
{

/// How the terminal engine failed to honour a sequence.
///
/// These map one-to-one onto the buckets `vtbackend::Screen` already reports to the `vt.parser` log
/// category, which makes them a zero-cost conformance oracle: every sequence a test program sends
/// that Contour does not fully implement announces itself here, with no golden file required.
enum class DiagnosticKind : std::uint8_t
{
    /// No `vtbackend::Function` matched — the sequence is not implemented at all, or is gated above
    /// the current conformance level.
    Unknown,

    /// A function matched but is deliberately not implemented (`ApplyResult::Unsupported`).
    Unsupported,

    /// A function matched but its parameters were rejected (`ApplyResult::Invalid`).
    Invalid,

    /// The parser's state machine rejected the input outright.
    ParserError,
};

/// One distinct diagnostic, with how many times it occurred.
struct Diagnostic
{
    DiagnosticKind kind;

    /// The offending sequence, as `vtbackend::Screen` rendered it.
    std::string sequence;

    /// Occurrence count across the run. A sequence sent in a loop should not dominate a report.
    size_t count = 1;
};

/// Collapses a run-order-dependent request id in a self-echoed reply to a stable placeholder.
///
/// With SRM reset (local echo on) the terminal echoes its own replies back through its own parser
/// (@see `vtbackend::Terminal::flushInput`). A DECRQCRA reply is `DCS <Pid> ! ~ <checksum> ST`, and a
/// test program numbers each read-back with a counter it increments across the whole run — so the Pid
/// drifts every time the number of read-backs changes. The parser has no handler for a reply it should
/// never receive, so it surfaces here as an "unknown" gap. Recording it by its exact Pid would make the
/// gap ratchet flap on every unrelated conformance fix; recording it by shape keeps the ratchet stable.
///
/// Only the DECRQCRA-reply shape `DCS <digits> ! ~` is rewritten (to `DCS <id> ! ~`); every other
/// sequence, including ones whose number is meaningful (a CSI mode, say), is returned unchanged.
///
/// @param sequence The offending sequence text, as the engine rendered it.
/// @return The sequence with a volatile request id folded to `<id>`, or the input unchanged.
[[nodiscard]] std::string canonicalizeSequence(std::string_view sequence);

/// Classifies one `vt.parser` log line.
///
/// This is the pure half of the oracle and is unit-tested directly, without any logging machinery.
///
/// @param line A formatted `vt.parser` message.
/// @return The diagnostic, or nullopt if the line is not one of the four failure buckets.
[[nodiscard]] std::optional<Diagnostic> classifyDiagnostic(std::string_view line);

/// Captures every `vt.parser` diagnostic emitted while it is alive.
///
/// Installs a sink and a raw formatter on the `vt.parser` category for its lifetime and restores the
/// previous ones on destruction, so it composes with the normal logging setup rather than fighting
/// it. Thread-safe: the terminal's parser runs on its own pump thread.
class DiagnosticsCollector
{
  public:
    DiagnosticsCollector();
    ~DiagnosticsCollector();

    DiagnosticsCollector(DiagnosticsCollector const&) = delete;
    DiagnosticsCollector& operator=(DiagnosticsCollector const&) = delete;
    DiagnosticsCollector(DiagnosticsCollector&&) = delete;
    DiagnosticsCollector& operator=(DiagnosticsCollector&&) = delete;

    /// @return The distinct diagnostics seen so far, deduplicated and counted, in first-seen order.
    [[nodiscard]] std::vector<Diagnostic> collected() const;

    /// Discards everything collected so far, e.g. between scenarios.
    void clear();

  private:
    void record(std::string_view line);

    mutable std::mutex _mutex;
    std::vector<Diagnostic> _diagnostics;
    std::unique_ptr<logstore::sink> _sink;
};

} // namespace vtconformance
