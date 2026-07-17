// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/CellFlags.h>
#include <vtbackend/LineFlags.h>
#include <vtbackend/primitives.h>

#include <string>
#include <variant>
#include <vector>

namespace vtbackend
{
class Terminal;
}

namespace vtconformance
{

/// Which planes a screen dump should carry.
///
/// A dump is the golden-file representation of a terminal screen. It is deliberately textual and
/// line-oriented so that a `diff` between a captured and a blessed dump reads like a description of
/// the defect rather than a wall of bytes.
struct DumpOptions
{
    /// Emit the attribute plane (one legend character per cell) and its legend.
    bool attributes = true;

    /// Emit per-line flags (double-width / double-height / wrapped).
    bool lineFlags = true;

    /// Emit the terminal mode plane (DECOM, DECAWM, IRM, ...).
    bool modes = true;

    /// Emit the cursor position and visibility.
    bool cursor = true;
};

/// One row of the mode plane: a terminal mode we consider conformance-relevant.
///
/// Adding a mode to a dump is adding a row here; no dump logic changes.
struct ModeProbe
{
    std::string_view mnemonic;

    /// Either an ANSI mode (`CSI Pm h`) or a DEC private mode (`CSI ? Pm h`).
    std::variant<vtbackend::AnsiMode, vtbackend::DECMode> mode;
};

/// The conformance-relevant modes recorded in every screen dump.
[[nodiscard]] std::vector<ModeProbe> const& dumpedModes() noexcept;

/// Renders @p terminal's active screen as a deterministic, human-readable golden dump.
///
/// The format is stable across runs and machine-independent: no timestamps, no pointers, no
/// hash-ordered containers. See `ScreenDump_test.cpp` for a worked example.
///
/// @param terminal The terminal whose active screen is captured.
/// @param options  Which planes to include.
/// @return The dump text, newline-terminated.
[[nodiscard]] std::string dumpScreen(vtbackend::Terminal const& terminal, DumpOptions const& options = {});

/// Renders the difference between two dumps as a unified-diff-like listing.
///
/// @param expected The blessed golden dump.
/// @param actual   The dump just captured.
/// @return An empty string if the dumps are equal, else a human-readable diff.
[[nodiscard]] std::string diffDumps(std::string_view expected, std::string_view actual);

} // namespace vtconformance
