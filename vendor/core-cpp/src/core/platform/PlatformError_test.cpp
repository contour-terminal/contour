// SPDX-License-Identifier: Apache-2.0
#include <core/platform/PlatformError.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace core::platform;

TEST_CASE("PlatformError.toString", "[platform]")
{
    CHECK(toString(PlatformError::ForkFailed) == "fork failed");
    CHECK(toString(PlatformError::ExecFailed) == "exec failed");
    CHECK(toString(PlatformError::WaitFailed) == "wait failed");
    CHECK(toString(PlatformError::ProgramNotFound) == "program not found");
    CHECK(toString(PlatformError::PipeCreationFailed) == "pipe creation failed");
    CHECK(toString(PlatformError::HandleDuplicationFailed) == "handle duplication failed");
    CHECK(toString(PlatformError::FileNotFound) == "file not found");
    CHECK(toString(PlatformError::PermissionDenied) == "permission denied");
    CHECK(toString(PlatformError::IoError) == "I/O error");
    CHECK(toString(PlatformError::SignalFailed) == "signal failed");
    CHECK(toString(PlatformError::SessionCreationFailed) == "session creation failed");
    CHECK(toString(PlatformError::ProcessGroupFailed) == "process group failed");
    CHECK(toString(PlatformError::TerminalControlFailed) == "terminal control failed");
    CHECK(toString(PlatformError::NotImplemented) == "not implemented");
    CHECK(toString(PlatformError::InvalidArgument) == "invalid argument");
}
