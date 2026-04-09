// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file SuppressWindowsDialogs.hpp
/// @brief Suppresses Windows GUI dialogs (assert, abort, crash) during test execution.
///
/// Call suppressWindowsDialogs() at the start of main() in every test executable
/// to prevent modal dialog boxes from blocking CI and interactive test runs.

#if defined(_WIN32)
    #include <cstdlib>

    #include <Windows.h>
    #include <crtdbg.h>
#endif

namespace crispy::testing
{

/// @brief Suppresses all Windows GUI dialogs that can appear during test execution.
///
/// This prevents the following dialogs from blocking test runs:
/// - CRT assert/error/warning dialogs (redirected to stderr)
/// - abort() message box
/// - Windows Error Reporting ("program has stopped working")
/// - Invalid parameter handler dialogs
inline void suppressWindowsDialogs()
{
#if defined(_WIN32)
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

} // namespace crispy::testing
