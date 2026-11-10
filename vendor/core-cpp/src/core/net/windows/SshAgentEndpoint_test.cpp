// SPDX-License-Identifier: Apache-2.0
#include <core/net/SshAgentEndpoint.hpp>
#include <core/testing/Environment.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("defaultSshAgentEndpoint is the Win32-OpenSSH pipe")
{
    CHECK(core::net::defaultSshAgentEndpoint(core::testing::FakeEnvironment {})
          == R"(\\.\pipe\openssh-ssh-agent)");
}
