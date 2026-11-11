// SPDX-License-Identifier: Apache-2.0
#include <core/net/SshAgentEndpoint.hpp>
#include <core/testing/Environment.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("defaultSshAgentEndpoint reads SSH_AUTH_SOCK")
{
    CHECK(core::net::defaultSshAgentEndpoint(
              core::testing::FakeEnvironment { { { "SSH_AUTH_SOCK", "/run/agent.sock" } } })
          == "/run/agent.sock");
    CHECK(core::net::defaultSshAgentEndpoint(core::testing::FakeEnvironment {}).empty());
}
