// SPDX-License-Identifier: Apache-2.0
#include <core/platform/MachineId.hpp>
#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace core::platform;
using core::platform::testing::InMemoryFileSystem;

TEST_CASE("MachineId.valid")
{
    auto const fileSystem =
        InMemoryFileSystem { { "/etc/machine-id", "0123456789ABCDEF0123456789abcdef\n" } };
    auto const id = readMachineId(fileSystem);
    REQUIRE(id.has_value());
    CHECK(*id == "0123456789abcdef0123456789abcdef");
}

TEST_CASE("MachineId.missing")
{
    auto const fileSystem = InMemoryFileSystem {};
    CHECK_FALSE(readMachineId(fileSystem).has_value());
}

TEST_CASE("MachineId.uninitialized")
{
    auto const fileSystem = InMemoryFileSystem { { "/etc/machine-id", "uninitialized\n" } };
    CHECK_FALSE(readMachineId(fileSystem).has_value());
}

TEST_CASE("MachineId.wrongLength")
{
    auto const tooShort = InMemoryFileSystem { { "/etc/machine-id", "0123456789abcdef0123456789abcde\n" } };
    CHECK_FALSE(readMachineId(tooShort).has_value());

    auto const tooLong = InMemoryFileSystem { { "/etc/machine-id", "0123456789abcdef0123456789abcdef0\n" } };
    CHECK_FALSE(readMachineId(tooLong).has_value());
}

TEST_CASE("MachineId.nonHexDigits")
{
    auto const fileSystem =
        InMemoryFileSystem { { "/etc/machine-id", "0123456789abcdef0123456789abcdeg\n" } };
    CHECK_FALSE(readMachineId(fileSystem).has_value());
}
