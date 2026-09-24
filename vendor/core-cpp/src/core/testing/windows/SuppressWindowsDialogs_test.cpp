// SPDX-License-Identifier: Apache-2.0
//
// What suppressWindowsDialogs() asks Windows Error Reporting for, read back from WER.
#include <core/testing/SuppressWindowsDialogs.hpp>

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>
#include <werapi.h>

TEST_CASE("suppressWindowsDialogs asks Windows Error Reporting for no UI", "[testing][windows]")
{
    core::testing::suppressWindowsDialogs();
    auto flags = DWORD { 0 };
    REQUIRE(WerGetFlags(GetCurrentProcess(), &flags) == S_OK);
    CHECK((flags & WER_FAULT_REPORTING_NO_UI) != 0);
}
