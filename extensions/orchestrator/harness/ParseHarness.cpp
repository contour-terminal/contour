// SPDX-License-Identifier: Apache-2.0
//
// The parse-budget harness: a WASI reactor that runs the core's `gh-prs` mapping (or, built with
// ORCH_PARSE_GLAB_MRS=1, its `glab-mrs` one) over a buffer the host wrote into its memory, so that the host
// can time the parse under the interpreter the extension will run in. It is a raw core module, not a
// component: no world, no wit-bindgen, just the exports the measurement needs.
//
// The host calls `_initialize` once, then `reserve(size)` for a buffer, writes the reply there, and calls
// `parse(address, size)` as often as it wants to time it. The buffer stays reserved for the instance's
// lifetime: the host drops the instance when it is done.
#include <cstdint>
#include <cstdlib>
#include <string_view>

#include <orchestrator/core/GitHub.hpp>
#include <orchestrator/core/GitLab.hpp>
#include <orchestrator/core/Model.hpp>

#ifndef ORCH_PARSE_GLAB_MRS
    #define ORCH_PARSE_GLAB_MRS 0
#endif

namespace
{
/// The mapping the build selected: `gh-prs`'s, or `glab-mrs`'s.
constexpr auto Mapping =
    ORCH_PARSE_GLAB_MRS != 0 ? &orchestrator::gitlab::changeRequests : &orchestrator::github::changeRequests;
} // namespace

extern "C"
{

    /// Reserves guest memory for the host to write a reply into.
    /// @param size The reply's size in bytes.
    /// @return The buffer's guest address; 0 if the memory could not grow to hold it.
    __attribute__((export_name("reserve"))) char* reserve(uint32_t size)
    {
        return static_cast<char*>(std::malloc(size));
    }

    /// Maps a `gh-prs` (or `glab-mrs`) reply, as the engine does on every poll.
    /// @param text The reply, at a guest address reserve() returned.
    /// @param size Its size in bytes.
    /// @return How many change requests it holds; -1 if the reply was refused.
    __attribute__((export_name("parse"))) int32_t parse(char const* text, uint32_t size)
    {
        auto const listed = Mapping(std::string_view { text, size }, orchestrator::OutputExtent::Whole);
        return listed ? static_cast<int32_t>(listed->items.size()) : -1;
    }

} // extern "C"
