// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
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

/// @return Every kind of memory object this platform can make: its own, and macOS's mapped shared memory,
///         which every POSIX platform has, so that its path is exercised wherever the tests run.
[[nodiscard]] std::vector<HandoverMemoryKind> availableKinds()
{
    auto kinds = std::vector<HandoverMemoryKind> {
        nativeHandoverMemoryKind(),
        HandoverMemoryKind { .object = HandoverMemoryObject::SharedMemory,
                             .access = HandoverMemoryAccess::Mapped },
    };
    #ifndef __APPLE__
    // Darwin's shared memory objects take no read or write: the other BSDs' kind, everywhere else.
    kinds.push_back(HandoverMemoryKind { .object = HandoverMemoryObject::SharedMemory,
                                         .access = HandoverMemoryAccess::Descriptor });
    #endif
    return kinds;
}
} // namespace

TEST_CASE("every kind of memory object reads back what was written, and nothing past it", "[handover]")
{
    auto const kind = GENERATE(Catch::Generators::from_range(availableKinds()));
    INFO(std::to_underlying(kind.object) << "/" << std::to_underlying(kind.access));
    auto memory = makeHandoverMemory(kind);
    // Sizes off a page boundary, where a platform that rounds the object's size up would read zeros past it.
    for (auto const size: { size_t { 0 }, size_t { 1 }, size_t { 4095 }, size_t { 300'001 } })
    {
        INFO(size);
        auto image = std::vector<uint8_t>(size);
        std::ranges::generate(image, [next = uint8_t { 1 }]() mutable { return next++; });
        auto const fd = memory->write(image);
        REQUIRE(fd.has_value());
        CHECK(isCloseOnExec(*fd));
        auto const back = memory->read(*fd);
        REQUIRE(back.has_value());
        CHECK(*back == image);
        CHECK(memory->read(*fd) == back);
        memory->discard(*fd);
    }
}

    #ifndef __APPLE__
// Forged with a pwrite, which Darwin's shared memory takes no more than the writer does.
TEST_CASE("a mapped memory object whose size header lies is refused", "[handover]")
{
    auto memory = makeHandoverMemory(HandoverMemoryKind { .object = HandoverMemoryObject::SharedMemory,
                                                          .access = HandoverMemoryAccess::Mapped });
    auto const fd = memory->write(std::vector<uint8_t> { 1, 2, 3 });
    REQUIRE(fd.has_value());
    // The header claims more than the object holds.
    auto const claimed = uint64_t { 1 } << 20;
    REQUIRE(::pwrite(*fd, &claimed, sizeof(claimed), 0) == static_cast<ssize_t>(sizeof(claimed)));
    CHECK(memory->read(*fd) == std::unexpected(HandoverError::MemoryUnavailable));
    memory->discard(*fd);
}
    #endif

TEST_CASE("read returns what write stored", "[handover]")
{
    auto memory = makeNativeHandoverMemory();
    auto image = std::vector<uint8_t>(300'000);
    std::ranges::generate(image, [next = uint8_t { 0 }]() mutable { return next++; });
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

TEST_CASE("a descriptor kept for a failed exec is close-on-exec again, and discarded closed", "[handover]")
{
    auto memory = makeNativeHandoverMemory();
    auto const fd = memory->write(std::vector<uint8_t> { 1, 2, 3 });
    REQUIRE(fd.has_value());
    REQUIRE(memory->keepAcrossExec(*fd).has_value());
    REQUIRE(memory->closeOnExecAgain(*fd).has_value());
    CHECK(isCloseOnExec(*fd));
    memory->discard(*fd);
    CHECK(::fcntl(*fd, F_GETFD) == -1);
    CHECK_FALSE(memory->closeOnExecAgain(*fd).has_value());
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
