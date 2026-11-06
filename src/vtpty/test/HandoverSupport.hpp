// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Whether this test binary can hand a PTY over at all: what the handover tests of vtpty and vthost ask
/// before they start one.

#ifdef _WIN32
    #include <vtpty/ConPty.hpp>
    #include <vtpty/PageSize.hpp>
#endif

#include <catch2/catch_test_macros.hpp>

namespace vtpty::testing
{

/// Skips the test where a PTY cannot be handed over. Every POSIX PTY can. On Windows only the bundled
/// conpty.dll's pseudoconsole can, whose handles alone are known, so the test is skipped where that one is
/// not beside the test binary (CONTOUR_EMBED_OPENCONSOLE=OFF). Called on the test's own thread, before any
/// session is spawned.
inline void requireHandoverCapable()
{
#ifdef _WIN32
    if (ConPty { PageSize { LineCount(24), ColumnCount(80) } }.consoleHost() != ConsoleHost::Bundled)
        SKIP("the bundled conpty.dll is not beside this test");
#endif
}

} // namespace vtpty::testing
