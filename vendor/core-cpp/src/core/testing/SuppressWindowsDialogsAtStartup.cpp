// SPDX-License-Identifier: Apache-2.0
//
// Calls core::testing::suppressWindowsDialogs() before main() runs, in every executable that links
// the core::testing_dialogs object library: every test binary (through core::testing_main) and any
// executable whose main() is not core-cpp's.
//
// It runs from static initialisation rather than from each main(), because a call that each main()
// has to remember is the defect: in endo, whose file this is, one test runner's main() had none, a
// test tripped a CRT assert under ctest, and the modal dialog held the run for 58 minutes until
// somebody clicked it away. tests/WindowsDialogCanary.cpp proves it reaches a main() that asks for
// nothing.

#ifdef _WIN32

    #include <core/testing/SuppressWindowsDialogs.hpp>

    #ifdef _MSC_VER
        // Run in the library initialisation phase, ahead of every ordinary static initializer, so an
        // assert in one of those reports to stderr too. C4073 is the warning that init_seg(lib)
        // raises by design, to say that it was used.
        #pragma warning(disable : 4073)
        #pragma init_seg(lib)
    #endif

namespace
{

struct SuppressWindowsDialogsAtStartup
{
    SuppressWindowsDialogsAtStartup() noexcept { core::testing::suppressWindowsDialogs(); }
};

SuppressWindowsDialogsAtStartup const suppressWindowsDialogsAtStartup {};

} // namespace

#endif
