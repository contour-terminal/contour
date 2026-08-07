// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Suppresses Windows GUI dialogs (assert, abort, crash) for non-interactive runs.
///
/// Call @c suppressWindowsDialogs() at the start of a test runner's `main()` so a
/// modal CRT assert/abort/crash dialog cannot block a headless run; the reports go
/// to stderr instead, so they stay visible.
///
/// `coro` keeps its own copy (used by `net`'s runner too) rather than using Contour's `crispy` one,
/// so the modules — including their test runners — stay dependency-free and can be
/// consumed by other projects unchanged. It is a handful of CRT calls, so the
/// duplication is cheaper than the coupling.

#ifdef _WIN32
    #include <cstdlib>

    #include <Windows.h>
    #include <crtdbg.h>
#endif

namespace coro::testing
{

/// Suppresses the Windows GUI dialogs that can block an unattended run:
/// CRT assert/error/warning reports (redirected to stderr), the `abort()` message
/// box, Windows Error Reporting, and the invalid-parameter handler dialog.
/// A no-op on every other platform.
inline void suppressWindowsDialogs()
{
#ifdef _WIN32
    // Redirect CRT debug reports (assert, error, warning) to stderr instead of showing dialogs.
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);

    // Prevent the abort() message box and Windows Error Reporting fault dialog.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);

    // Suppress the Windows "critical error" and "program has stopped working" dialogs.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

    // Suppress the invalid parameter handler dialog (e.g. from invalid CRT function arguments).
    _set_invalid_parameter_handler([]([[maybe_unused]] wchar_t const* expression,
                                      [[maybe_unused]] wchar_t const* function,
                                      [[maybe_unused]] wchar_t const* file,
                                      [[maybe_unused]] unsigned int line,
                                      [[maybe_unused]] uintptr_t reserved) {});
#endif
}

} // namespace coro::testing
