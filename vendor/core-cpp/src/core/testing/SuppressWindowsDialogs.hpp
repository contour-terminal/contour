// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Suppresses the Windows dialogs (CRT assert, abort, crash) that block an unattended run.
///
/// A failed assert(), an abort() or a crash in a Windows Debug build opens a modal dialog
/// and waits. Under ctest nobody is there to click it, so a test that should fail in a
/// second holds the run until its timeout instead. suppressWindowsDialogs() sends the
/// reports to stderr, where they stay visible, and lets the process exit.
///
/// core::testing_main calls it first thing in main(), and SuppressWindowsDialogsAtStartup.cpp
/// (the core::testing_dialogs object library) calls it during static initialisation, for an
/// executable whose main() is not core-cpp's.
///
/// This merges four copies that had drifted: contour's crispy and coro copies, endo's
/// testing copy and fastcached's WindowsErrorPopups.hpp. The union is kept: every CRT report
/// type, the abort() message box and fault report, the invalid-parameter handler, and the
/// OS error modes including SEM_NOOPENFILEERRORBOX. Unlike those copies it is defined out of
/// line, so including this header does not include <Windows.h>.

namespace core::testing
{

/// @brief Suppresses every Windows dialog that can block an unattended run.
///
/// - CRT assert, error and warning reports go to stderr instead of a dialog.
/// - abort() writes its message to stderr (a Debug CRT's; a Release UCRT writes none), shows no
///   message box and asks Windows Error Reporting for nothing.
/// - Windows Error Reporting is asked for no UI for a fault in this process
///   (`WerSetFlags(WER_FAULT_REPORTING_NO_UI)`).
/// - An invalid argument to a CRT function returns an error instead of opening a dialog.
/// - General-protection faults, critical errors and open-file errors show no OS dialog.
///
/// A no-op on every other platform.
void suppressWindowsDialogs() noexcept;

} // namespace core::testing
