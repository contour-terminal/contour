// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <numeric>
#include <string>
#include <vector>

#include <vthost/handover/HandoverMemory.hpp>

#ifndef _WIN32
    #include <fcntl.h>
    #include <unistd.h>
#endif

using namespace vthost::handover;

#ifndef _WIN32

namespace
{
[[nodiscard]] bool isCloseOnExec(int fd)
{
    auto const flags = ::fcntl(fd, F_GETFD);
    REQUIRE(flags >= 0);
    return (flags & FD_CLOEXEC) != 0;
}
} // namespace

TEST_CASE("read returns what write stored", "[handover]")
{
    auto memory = makeNativeHandoverMemory();
    auto image = std::vector<uint8_t>(300'000);
    std::ranges::iota(image, uint8_t { 0 });
    auto const fd = memory->write(image);
    REQUIRE(fd.has_value());
    auto const back = memory->read(*fd);
    REQUIRE(back.has_value());
    CHECK(*back == image);
    // Reading does not consume it: the same descriptor reads again.
    CHECK(memory->read(*fd) == back);
    ::close(*fd);
}

TEST_CASE("an empty image is stored and read back", "[handover]")
{
    auto memory = makeNativeHandoverMemory();
    auto const fd = memory->write({});
    REQUIRE(fd.has_value());
    auto const back = memory->read(*fd);
    REQUIRE(back.has_value());
    CHECK(back->empty());
    ::close(*fd);
}

TEST_CASE("reading a descriptor that is no memory object is MemoryUnavailable", "[handover]")
{
    auto memory = makeNativeHandoverMemory();
    auto const back = memory->read(-1);
    REQUIRE_FALSE(back.has_value());
    CHECK(back.error() == HandoverError::MemoryUnavailable);
}

TEST_CASE("the descriptor survives an exec only after keepAcrossExec", "[handover]")
{
    auto memory = makeNativeHandoverMemory();
    auto const fd = memory->write(std::vector<uint8_t> { 1, 2, 3 });
    REQUIRE(fd.has_value());
    CHECK(isCloseOnExec(*fd));
    REQUIRE(memory->keepAcrossExec(*fd).has_value());
    CHECK_FALSE(isCloseOnExec(*fd));
    ::close(*fd);
}

    #ifdef __linux__
TEST_CASE("native memory has no name on disk", "[handover]")
{
    auto memory = makeNativeHandoverMemory();
    auto const fd = memory->write(std::vector<uint8_t> { 1 });
    REQUIRE(fd.has_value());
    auto link = std::string(256, '\0');
    auto const length =
        ::readlink(("/proc/self/fd/" + std::to_string(*fd)).c_str(), link.data(), link.size());
    REQUIRE(length > 0);
    link.resize(static_cast<size_t>(length));
    CHECK(link.starts_with("/memfd:"));
    ::close(*fd);
}
    #endif

#else

TEST_CASE("native memory is not supported on Windows", "[handover]")
{
    auto memory = makeNativeHandoverMemory();
    auto const fd = memory->write({});
    REQUIRE_FALSE(fd.has_value());
    CHECK(fd.error() == HandoverError::NotSupported);
}

#endif
