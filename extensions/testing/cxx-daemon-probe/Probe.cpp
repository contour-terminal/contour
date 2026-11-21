// SPDX-License-Identifier: Apache-2.0
//
// The C++ daemon-world probe: a test guest built with the WASI C++ toolchain, the way the agent
// orchestrator's guest is, with no bindings. It declares the three imports it calls and the world's exports
// by hand, in the Canonical ABI's flat form, and carries its manifest in a custom section written by
// module-level assembly (the `section` attribute produces nothing for a custom section).
//
// `load` logs "cxx-probe loaded"; `update` reads `now` and publishes the summary "probe" with the monotonic
// milliseconds as its detail.
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

asm(R"(.section ".custom_section.terminal-extension-manifest","",@
.ascii "name = cxx-probe\nversion = 2\nplacement daemon\n"
.text
)");

namespace probe
{

/// WIT `clock-reading`, as `now` stores it at the address it is given.
struct ClockReading
{
    uint64_t monotonicMs; ///< Milliseconds on a clock that never goes back.
    uint64_t wallMs;      ///< Milliseconds since the Unix epoch.
};

/// WIT `result<_, host-error>`, as an import stores it at the address it is given.
struct HostResult
{
    uint8_t isError; ///< 0: ok; 1: the error below.
    uint8_t error;   ///< The `host-error` case.
};

/// WIT `log-level`.
enum class LogLevel : int32_t
{
    Trace,
    Debug,
    Info,
    Warn,
    Error,
};

// The daemon world's imports this probe calls. Their types have linkage (a named namespace), as a function
// defined by the host must.
__attribute__((import_module("terminal-extensions:daemon/host@0.1.0"), import_name("log"))) void hostLog(
    LogLevel level, char const* message, std::size_t messageLength);
__attribute__((import_module("terminal-extensions:daemon/host@0.1.0"), import_name("now"))) void hostNow(
    ClockReading* reading);
__attribute__((import_module("terminal-extensions:daemon/host@0.1.0"), import_name("summary-set"))) void
hostSummarySet(char const* shortText,
               std::size_t shortLength,
               char const* detail,
               std::size_t detailLength,
               HostResult* result);

} // namespace probe

using namespace probe;

/// The world's `load(config-json, info)`: logs that it was loaded; the configuration and the host's info
/// are not read.
[[clang::export_name("load")]] void load(char const* /*configJson*/,
                                         std::size_t /*configJsonLength*/,
                                         void const* /*roots*/,
                                         std::size_t /*rootCount*/,
                                         char const* /*home*/,
                                         std::size_t /*homeLength*/,
                                         int32_t /*utcOffsetMinutes*/,
                                         void const* /*selfPrograms*/,
                                         std::size_t /*selfProgramCount*/)
{
    constexpr auto Message = std::string_view { "cxx-probe loaded" };
    hostLog(LogLevel::Info, Message.data(), Message.size());
}

/// The world's `update(hint)`: whatever the hint, publishes the summary "probe" with the monotonic
/// milliseconds `now` reads as its detail.
[[clang::export_name("update")]] void update(int32_t /*hint*/)
{
    auto reading = ClockReading {};
    hostNow(&reading);
    auto digits = std::array<char, 24> {};
    auto const [end, error] =
        std::to_chars(digits.data(), digits.data() + digits.size(), reading.monotonicMs);
    if (error != std::errc {})
        return;
    constexpr auto Short = std::string_view { "probe" };
    auto result = HostResult {};
    hostSummarySet(
        Short.data(), Short.size(), digits.data(), static_cast<std::size_t>(end - digits.data()), &result);
}

/// The Canonical ABI's allocator, through which the host hands the guest the arguments of its exports.
/// @param previous What is grown, or null for a new allocation.
/// @param alignment The alignment the allocation needs; `malloc`'s suffices for every type the world passes.
/// @param size The bytes needed.
/// @return The allocation; @p alignment itself, a non-null address nothing is stored at, for no bytes.
[[clang::export_name("cabi_realloc")]] void* cabiRealloc(void* previous,
                                                         std::size_t /*previousSize*/,
                                                         std::size_t alignment,
                                                         std::size_t size)
{
    if (size == 0)
        return reinterpret_cast<void*>(alignment);
    return std::realloc(previous, size);
}
