// SPDX-License-Identifier: Apache-2.0
//
// A `DetachedTask`'s ramp must not touch the coroutine frame once the coroutine has suspended:
// whoever it suspended into -- a pool thread, an event loop, or the `await_suspend` itself -- may
// run it to its end and free the frame before the ramp returns (core-cpp#51).
//
// Reading freed memory rarely faults on its own, so this binary replaces the global allocation
// functions: while a case asks for it, the calling thread's allocations -- the coroutine frames
// among them -- come from pages of their own, and freeing one makes its pages inaccessible, so a
// read of a freed frame faults at the instruction that makes it. A binary of its own, because the
// replacement would reach every case linked beside it; not under a sanitizer, whose runtime defines
// the allocation functions itself.
#include <core/async/DetachedTask.hpp>
#include <core/async/IExecutor.hpp>
#include <core/async/ParkedWork.hpp>
#include <core/async/ResumeOn.hpp>
#include <core/testing/ReplacedGlobalAllocation.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <ranges>
#include <span>
#include <utility>

#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
    #include <core/async/ThreadPoolExecutor.hpp>

    #include <thread>
#endif

// The page allocator is the one platform difference here, and it is test scaffolding: Windows'
// VirtualAlloc/VirtualProtect, POSIX's mmap/mprotect, and plain malloc where there is neither
// (WebAssembly), which guards nothing.
#ifdef _WIN32
    #include <windows.h>
#elifndef __EMSCRIPTEN__
    #include <sys/mman.h>

    #include <unistd.h>
#endif

namespace
{

/// Whether the calling thread's allocations come from guarded pages right now.
thread_local bool guardAllocations = false;

/// Where a block's storage came from.
enum class Backing : std::uint8_t
{
    Heap,  ///< `malloc`, and back to `free`.
    Pages, ///< Pages of its own, revoked rather than freed.
};

/// What precedes every block this binary hands out: enough to give its storage back. Every block
/// has one, so a free never reads in front of storage it did not lay out -- in front of a large
/// `malloc` block may be an unmapped page.
struct BlockHeader
{
    Backing backing;
    void* base;
    std::size_t length;
    std::uint64_t padding;
};
static_assert(sizeof(BlockHeader) % alignof(std::max_align_t) == 0);

/// @return The size of a page.
std::size_t pageSize() noexcept
{
#ifdef _WIN32
    auto info = SYSTEM_INFO {};
    GetSystemInfo(&info);
    return info.dwPageSize;
#elifndef __EMSCRIPTEN__
    return static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
#else
    return 64 * 1024;
#endif
}

/// Maps @p length bytes of fresh, readable and writable pages.
void* mapPages(std::size_t length) noexcept
{
#ifdef _WIN32
    return VirtualAlloc(nullptr, length, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#elifndef __EMSCRIPTEN__
    auto* const pages = mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return pages == MAP_FAILED ? nullptr : pages;
#else
    return std::malloc(length);
#endif
}

/// Makes @p length bytes at @p base fault on any access, for good: they are never handed out again.
void revokePages([[maybe_unused]] void* base, [[maybe_unused]] std::size_t length) noexcept
{
#ifdef _WIN32
    auto previous = DWORD {};
    VirtualProtect(base, length, PAGE_NOACCESS, &previous);
#elifndef __EMSCRIPTEN__
    mprotect(base, length, PROT_NONE);
#endif
}

/// Gives @p length bytes at @p base back to the system, revoked or not.
void unmapPages(void* base, [[maybe_unused]] std::size_t length) noexcept
{
#ifdef _WIN32
    VirtualFree(base, 0, MEM_RELEASE);
#elifndef __EMSCRIPTEN__
    munmap(base, length);
#else
    std::free(base);
#endif
}

/// A revoked block, kept so a case can give its address space back once nothing can read it.
struct RevokedBlock
{
    void* base;
    std::size_t length;
};

/// Room for every block the cases here revoke: the pool case revokes two or three per flow. A
/// block past it stays mapped, which costs address space and nothing else.
constexpr auto RevokedCapacity = std::size_t { 16384 };

/// The revoked blocks not yet given back, filled from any thread through @c revokedCount.
std::array<RevokedBlock, RevokedCapacity> revokedBlocks {};

/// How many blocks have been revoked since they were last given back.
std::atomic<std::size_t> revokedCount { 0 };

/// Unmaps every revoked block. Called only once nothing can free another, at the end of a case.
///
/// Without it every guarded frame stays mapped for the life of the process: the pool case alone
/// maps a page or more per flow, 8 MB of address space on x86-64 and 32 MB with 16 KB pages, and
/// a revoked Windows page keeps its commit charge.
/// @return How many blocks were revoked, which says the guard was engaged at all.
std::size_t returnRevokedPages() noexcept
{
    auto const revoked = revokedCount.exchange(0);
    for (auto const& block: std::span { revokedBlocks }.first(std::min(revoked, RevokedCapacity)))
        unmapPages(block.base, block.length);
    return revoked;
}

/// Serves an allocation from guarded pages, or from `malloc`, behind its header.
void* allocate(std::size_t size)
{
    auto const guarded = guardAllocations;
    auto const page = pageSize();
    auto const length =
        guarded ? (sizeof(BlockHeader) + size + page - 1) / page * page : sizeof(BlockHeader) + size;
    auto* const base = guarded ? mapPages(length) : std::malloc(length);
    if (base == nullptr)
        throw std::bad_alloc {};
    auto const header = BlockHeader {
        .backing = guarded ? Backing::Pages : Backing::Heap, .base = base, .length = length, .padding = 0
    };
    std::memcpy(base, &header, sizeof header);
    return static_cast<std::byte*>(base) + sizeof(BlockHeader);
}

/// Gives @p storage back: a guarded block's pages are revoked, anything else goes back to `free`.
void release(void* storage) noexcept
{
    if (storage == nullptr)
        return;
    auto header = BlockHeader {};
    std::memcpy(&header, static_cast<std::byte*>(storage) - sizeof(BlockHeader), sizeof header);
    if (header.backing == Backing::Heap)
    {
        std::free(header.base);
        return;
    }
    revokePages(header.base, header.length);
    if (auto const slot = revokedCount.fetch_add(1); slot < RevokedCapacity)
        revokedBlocks.at(slot) = RevokedBlock { .base = header.base, .length = header.length };
}

/// Guards the calling thread's allocations for as long as it lives.
class GuardedFrames final
{
  public:
    GuardedFrames() noexcept { guardAllocations = true; }
    GuardedFrames(GuardedFrames const&) = delete;
    GuardedFrames(GuardedFrames&&) = delete;
    GuardedFrames& operator=(GuardedFrames const&) = delete;
    GuardedFrames& operator=(GuardedFrames&&) = delete;
    ~GuardedFrames() { guardAllocations = false; }
};

/// Resumes the awaiting coroutine inside its own `await_suspend`, so it runs to its end -- and frees
/// its frame -- before the ramp that started it returns.
struct RunToEndNow
{
    [[nodiscard]] bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> self) const { self.resume(); }
    void await_resume() const noexcept {}
};

/// Increments @p ran after an await that finishes the coroutine before the ramp returns.
core::async::DetachedTask finishedInsideItsAwait(int* ran)
{
    co_await RunToEndNow {};
    ++*ran;
}

} // namespace

// The replaced global allocation functions are in ReplacedGlobalAllocation.cpp, which forwards
// them here (its header says why they are not in this file).
void* core::testing::replacedAllocate(std::size_t size)
{
    return allocate(size);
}

void core::testing::replacedRelease(void* storage) noexcept
{
    release(storage);
}

TEST_CASE("A DetachedTask finished inside its first await is not read by its ramp afterwards",
          "[DetachedTask][lifetime]")
{
    // Deterministic: the frame is freed inside the await_suspend call, before the ramp returns. A
    // ramp that reads its return object back from the frame -- clang-cl at -O0 returning the empty
    // DetachedTask in AL (core-cpp#51) -- faults on the revoked page.
    auto ran = 0;
    {
        auto const guard = GuardedFrames {};
        finishedInsideItsAwait(&ran);
    }
    auto const guarded = returnRevokedPages();
    CHECK(ran == 1);
    // The coroutine starts and ends inside this call, so a compiler may elide its heap frame
    // altogether (emsdk's latest clang does, in Release). Then there is no freed frame for a ramp to
    // read, and this case observed nothing.
    if (guarded == 0)
        SKIP("the frame allocation was elided, so no frame was guarded or freed");
}

#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)

namespace
{

/// An executor that hands what it is given to a thread of its own and returns only once that thread
/// has run it: the hop onto a loop on another thread, with the loop always winning the race.
class RunsElsewhereFirst final: public core::async::IExecutor
{
  public:
    using IExecutor::submit;

    void submit(std::coroutine_handle<> handle) override
    {
        submit(core::async::ParkedWork { .resume = handle });
    }

    void submit(core::async::ParkedWork work) override
    {
        auto entry = core::async::detail::Parked { std::move(work) };
        auto worker = std::thread { [&entry] { entry.resume(); } };
        worker.join();
    }
};

/// fastcached's shutdown shape (`FrameServer::Shutdown`, the Raft peer server's shutdown,
/// `RaftPeerTransport::CloseSockets`): hop onto the loop from whatever thread asked, and finish
/// there -- closing what the loop owns -- while the caller's ramp is still returning.
core::async::DetachedTask closeOnLoop(core::async::IExecutor* loop, std::atomic<int>* closed)
{
    co_await core::async::ResumeOn { *loop };
    closed->fetch_add(1);
}

} // namespace

TEST_CASE("A DetachedTask that hops onto another thread which finishes it is not read by its ramp",
          "[DetachedTask][lifetime][threads]")
{
    auto loop = RunsElsewhereFirst {};
    auto closed = std::atomic<int> { 0 };
    {
        auto const guard = GuardedFrames {};
        closeOnLoop(&loop, &closed);
    }
    CHECK(returnRevokedPages() >= std::size_t { 1 });
    CHECK(closed.load() == 1);
}

TEST_CASE("DetachedTasks hopping onto a pool that finishes them at teardown are not read by their ramps",
          "[DetachedTask][lifetime][threads]")
{
    // The same shape with the race left to the scheduler, as it is in fastcached: the pool may run a
    // flow to its end before the ramp on this thread has returned, or after. Every flow's frame is
    // guarded, so any ramp that loses the race and reads its frame faults.
    static constexpr auto Flows = 2000;
    auto closed = std::atomic<int> { 0 };
    {
        auto pool = core::async::ThreadPoolExecutor { 2 };
        for ([[maybe_unused]] auto const flow: std::views::iota(0, Flows))
        {
            auto const guard = GuardedFrames {};
            closeOnLoop(&pool, &closed);
        }
    } // teardown: the pool finishes what is still queued, then joins
    CHECK(returnRevokedPages() >= std::size_t { Flows }); // every flow's frame was guarded
    CHECK(closed.load() == Flows);
}

#endif
