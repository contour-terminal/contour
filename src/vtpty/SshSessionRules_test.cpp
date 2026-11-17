// SPDX-License-Identifier: Apache-2.0
#include <vtpty/SshSessionRules.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cerrno>

using namespace vtpty;

TEST_CASE("a read of an ended session asks for injected text before it reports the end", "[ssh]")
{
    STATIC_CHECK(sshClosedReadErrno(InjectedRead::Pending) == EINTR);
    STATIC_CHECK(sshClosedReadErrno(InjectedRead::Empty) == EPIPE);
}
