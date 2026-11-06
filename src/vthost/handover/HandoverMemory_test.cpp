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
    #include <sys/mman.h>

    #include <cstring>

    #include <fcntl.h>
    #include <unistd.h>
#else
    #include <cstddef>
    #include <cstring>
    #include <ranges>

    #include <Windows.h>
#endif

using namespace vthost::handover;

namespace
{

#ifndef _WIN32
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

/// @return Every kind whose image carries its length ahead of it: the mapped one.
[[nodiscard]] std::vector<HandoverMemoryKind> lengthPrefixedKinds()
{
    return { HandoverMemoryKind { .object = HandoverMemoryObject::SharedMemory,
                                  .access = HandoverMemoryAccess::Mapped } };
}

/// Overwrites the length an image's memory object carries ahead of it.
void forgeLengthPrefix(vtpty::CrossingHandle handle, uint64_t length)
{
    auto* const view =
        ::mmap(nullptr, sizeof(length), PROT_READ | PROT_WRITE, MAP_SHARED, handle.native(), 0);
    REQUIRE(view != MAP_FAILED);
    std::memcpy(view, &length, sizeof(length));
    ::munmap(view, sizeof(length));
}
#else
/// @return The one kind Windows has: an unnamed mapping.
[[nodiscard]] std::vector<HandoverMemoryKind> availableKinds()
{
    return { nativeHandoverMemoryKind() };
}

/// @return The one kind Windows has, which carries the image's length ahead of it.
[[nodiscard]] std::vector<HandoverMemoryKind> lengthPrefixedKinds()
{
    return availableKinds();
}

/// Overwrites the length an image's mapping carries ahead of it.
void forgeLengthPrefix(vtpty::CrossingHandle handle, uint64_t length)
{
    auto* const view = MapViewOfFile(handle.native(), FILE_MAP_WRITE, 0, 0, sizeof(length));
    REQUIRE(view != nullptr);
    std::memcpy(view, &length, sizeof(length));
    UnmapViewOfFile(view);
}
#endif

/// @return @p size bytes of a pattern that repeats only every 251 bytes, off any page boundary.
[[nodiscard]] std::vector<uint8_t> patternOf(size_t size)
{
    auto bytes = std::vector<uint8_t>(size);
    std::ranges::generate(bytes, [next = 0U]() mutable { return static_cast<uint8_t>(next++ % 251); });
    return bytes;
}

} // namespace

TEST_CASE("an image is read where it was written", "[handover]")
{
    auto const kind = GENERATE(Catch::Generators::from_range(availableKinds()));
    INFO(std::to_underlying(kind.object) << "/" << std::to_underlying(kind.access));
    auto memory = makeHandoverMemory(kind);
    constexpr auto Size = size_t { 1 } << 20;
    auto const handle = [&] {
        // The writer's own buffer is gone before anything is read.
        auto const image = patternOf(Size);
        return memory->write(image);
    }();
    REQUIRE(handle.has_value());
    auto const view = memory->read(*handle);
    REQUIRE(view.has_value());
    CHECK(std::ranges::equal(view->bytes(), patternOf(Size)));
    // The view holds what it reads by itself: the memory object's handle may go first.
    memory->discard(*handle);
    CHECK(std::ranges::equal(view->bytes(), patternOf(Size)));
}

TEST_CASE("a length prefix longer than the mapping is truncated", "[handover]")
{
    auto const kind = GENERATE(Catch::Generators::from_range(lengthPrefixedKinds()));
    auto memory = makeHandoverMemory(kind);
    auto const handle = memory->write(std::vector<uint8_t> { 1, 2, 3 });
    REQUIRE(handle.has_value());
    forgeLengthPrefix(*handle, uint64_t { 1 } << 20);
    CHECK(memory->read(*handle) == std::unexpected(HandoverError::Truncated));
    memory->discard(*handle);
}

TEST_CASE("a length above the image limit is truncated", "[handover]")
{
    auto const kind = GENERATE(Catch::Generators::from_range(lengthPrefixedKinds()));
    auto memory = makeHandoverMemory(kind);
    auto const handle = memory->write(std::vector<uint8_t> { 1, 2, 3 });
    REQUIRE(handle.has_value());
    forgeLengthPrefix(*handle, MaxHandoverImageSize + 1);
    CHECK(memory->read(*handle) == std::unexpected(HandoverError::Truncated));
    memory->discard(*handle);
}

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
        CHECK(std::ranges::equal(back->bytes(), image));
        auto const again = memory->read(*fd);
        REQUIRE(again.has_value());
        CHECK(std::ranges::equal(again->bytes(), image));
        memory->discard(*fd);
    }
}

TEST_CASE("read returns what write stored", "[handover]")
{
    auto memory = makeNativeHandoverMemory();
    auto image = std::vector<uint8_t>(300'000);
    std::ranges::generate(image, [next = uint8_t { 0 }]() mutable { return next++; });
    auto const fd = memory->write(image);
    REQUIRE(fd.has_value());
    auto const back = memory->read(*fd);
    REQUIRE(back.has_value());
    CHECK(std::ranges::equal(back->bytes(), image));
    // Reading does not consume it: the same descriptor reads again.
    auto const again = memory->read(*fd);
    REQUIRE(again.has_value());
    CHECK(std::ranges::equal(again->bytes(), image));
    ::close(fd->native());
}

TEST_CASE("an empty image is stored and read back", "[handover]")
{
    auto memory = makeNativeHandoverMemory();
    auto const fd = memory->write({});
    REQUIRE(fd.has_value());
    auto const back = memory->read(*fd);
    REQUIRE(back.has_value());
    CHECK(back->bytes().empty());
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
    CHECK(std::ranges::equal(back->bytes(), image));
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
