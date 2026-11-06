// SPDX-License-Identifier: Apache-2.0
#include <vtpty/test/HandleTesting.hpp>

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
#else
    #include <cstddef>
    #include <cstring>
    #include <ranges>

    #include <Windows.h>
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
        CHECK(isCloseOnExec(fd->native()));
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
    REQUIRE(::pwrite(fd->native(), &claimed, sizeof(claimed), 0) == static_cast<ssize_t>(sizeof(claimed)));
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
    ::close(fd->native());
}

TEST_CASE("an empty image is stored and read back", "[handover]")
{
    auto memory = makeNativeHandoverMemory();
    auto const fd = memory->write({});
    REQUIRE(fd.has_value());
    auto const back = memory->read(*fd);
    REQUIRE(back.has_value());
    CHECK(back->empty());
    ::close(fd->native());
}

TEST_CASE("reading a descriptor that is no memory object is MemoryUnavailable", "[handover]")
{
    auto memory = makeNativeHandoverMemory();
    auto const back = memory->read(vtpty::CrossingHandle {});
    REQUIRE_FALSE(back.has_value());
    CHECK(back.error() == HandoverError::MemoryUnavailable);
}

TEST_CASE("the descriptor survives an exec only after keepAcrossExec", "[handover]")
{
    auto memory = makeNativeHandoverMemory();
    auto const fd = memory->write(std::vector<uint8_t> { 1, 2, 3 });
    REQUIRE(fd.has_value());
    CHECK(isCloseOnExec(fd->native()));
    REQUIRE(memory->keepAcrossExec(*fd).has_value());
    CHECK_FALSE(isCloseOnExec(fd->native()));
    ::close(fd->native());
}

TEST_CASE("a descriptor kept for a failed exec is close-on-exec again, and discarded closed", "[handover]")
{
    auto memory = makeNativeHandoverMemory();
    auto const fd = memory->write(std::vector<uint8_t> { 1, 2, 3 });
    REQUIRE(fd.has_value());
    REQUIRE(memory->keepAcrossExec(*fd).has_value());
    REQUIRE(memory->closeOnExecAgain(*fd).has_value());
    CHECK(isCloseOnExec(fd->native()));
    memory->discard(*fd);
    CHECK_FALSE(vtpty::testing::isOpen(*fd));
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
        ::readlink(("/proc/self/fd/" + std::to_string(fd->native())).c_str(), link.data(), link.size());
    REQUIRE(length > 0);
    link.resize(static_cast<size_t>(length));
    CHECK(link.starts_with("/memfd:"));
    ::close(fd->native());
}
    #endif

#else

namespace
{
/// @return Whether a process this one starts with @p handle in its handle list inherits it.
[[nodiscard]] bool isInheritable(vtpty::CrossingHandle handle)
{
    auto flags = DWORD {};
    REQUIRE(GetHandleInformation(handle.native(), &flags) != FALSE);
    return (flags & HANDLE_FLAG_INHERIT) != 0;
}
} // namespace

TEST_CASE("a mapping reads back what was written, and nothing past it", "[handover]")
{
    // Around a page and beyond it: the mapping reports its size rounded up to one.
    auto const size = GENERATE(std::size_t { 0 }, 1, 4087, 4088, 4089, 4096, 70'000);
    auto image = std::vector<uint8_t>(size);
    for (auto const index: std::views::iota(std::size_t { 0 }, size))
        image[index] = static_cast<uint8_t>(index * 7 + 3);
    auto memory = makeNativeHandoverMemory();
    auto const fd = memory->write(image);
    REQUIRE(fd.has_value());
    auto const back = memory->read(*fd);
    REQUIRE(back.has_value());
    CHECK(*back == image);
    memory->discard(*fd);
}

TEST_CASE("reading a handle that is no mapping is MemoryUnavailable", "[handover]")
{
    auto memory = makeNativeHandoverMemory();
    auto* const event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    REQUIRE(event != nullptr);
    auto const back = memory->read(vtpty::CrossingHandle::of(event));
    CloseHandle(event);
    REQUIRE_FALSE(back.has_value());
    CHECK(back.error() == HandoverError::MemoryUnavailable);
}

TEST_CASE("a mapping whose length lies is refused", "[handover]")
{
    // A whole page, its first eight bytes claiming far more.
    auto* const mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096, nullptr);
    REQUIRE(mapping != nullptr);
    auto* const view = static_cast<uint8_t*>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, 0));
    REQUIRE(view != nullptr);
    auto const lie = uint64_t { 1 } << 20;
    std::memcpy(view, &lie, sizeof(lie));
    UnmapViewOfFile(view);

    auto memory = makeNativeHandoverMemory();
    auto const back = memory->read(vtpty::CrossingHandle::of(mapping));
    CloseHandle(mapping);
    REQUIRE_FALSE(back.has_value());
    CHECK(back.error() == HandoverError::Truncated);
}

// Only the successor's launch, which names it in its handle list, may take it: no program the daemon starts
// meanwhile, nor after a launch that failed.
TEST_CASE("a mapping is never inheritable", "[handover]")
{
    auto memory = makeNativeHandoverMemory();
    auto const fd = memory->write(std::vector<uint8_t> { 1, 2, 3 });
    REQUIRE(fd.has_value());
    CHECK_FALSE(isInheritable(*fd));
    memory->discard(*fd);
}

#endif
