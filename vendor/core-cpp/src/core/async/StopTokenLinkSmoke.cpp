// SPDX-License-Identifier: Apache-2.0
//
// A program that links core::async and nothing else, and drives the StopToken fallback.
//
// The fallback's stop state synchronises with a std::mutex, a std::condition_variable and
// std::this_thread::get_id() wherever core-cpp has threads at all (StopToken.hpp's StopStateSync).
// core::async is an INTERFACE target, so the only thing that can put Threads::Threads on this link
// line is core::async's own usage requirement -- which is the point of this program. What a
// consumer writes is `target_link_libraries(app PRIVATE core::async)`
// (docs/getting-started/cpm.md, .agent/guides/consumer-migration.md), and where pthread is a
// library of its own and the fallback branch is taken -- libc++ before 20 without
// -fexperimental-library, so FreeBSD 15 and AppleClang 17 -- that link failed while the module
// declared no dependency. core-cpp's own test binaries never showed it: they link
// core::testing_main, which brings core::base, which links Threads.
//
// It runs as well as links, so the ctest run also says the fallback works with nothing behind it.

#include <core/async/StopToken.hpp>

#include <cstdio>

static_assert(CORE_ASYNC_STOP_TOKEN_IS_STD == 0,
              "this program is built with CORE_ASYNC_FORCE_STOP_TOKEN_FALLBACK, so that it links "
              "what the fallback needs rather than what <stop_token> needs");

namespace
{

/// Records that it ran.
struct NoteStop
{
    bool* ran = nullptr;

    void operator()() const noexcept { *ran = true; }
};

} // namespace

int main()
{
    auto source = core::async::StopSource {};
    auto ran = false;
    auto const registration =
        core::async::StopCallback<NoteStop> { source.get_token(), NoteStop { .ran = &ran } };

    if (!source.request_stop() || !ran || !source.get_token().stop_requested())
    {
        std::puts("core-cpp-async-link-smoke: the StopToken fallback did not run its callback");
        return 1;
    }
    std::puts("core-cpp-async-link-smoke: linked core::async alone and ran the StopToken fallback");
    return 0;
}
