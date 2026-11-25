// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The names a named shared memory object is made under, where the system has no anonymous one (macOS's
/// shm_open). The name lives only until the object is open -- it is unlinked at once -- but another process
/// of the same user can guess or squat it meanwhile, so each name is random and a taken one is tried again.
/// Std-only, so that the decision is tested where shm_open is not used.

#include <cerrno>
#include <cstdint>
#include <expected>
#include <format>
#include <ranges>
#include <string>

namespace vtextension::wasm::remote::transport
{

/// How many names an object is tried under before its creation fails: a taken name is someone else's, and
/// eight random ones all taken is someone squatting the namespace, not chance.
inline constexpr auto SharedObjectNameAttempts = 8;

/// How many bits of a suffix a name keeps: 12 hex digits, so that `/contour-eh-<pid>-<suffix>` stays within
/// macOS's 31 characters (PSHMNAMLEN) for any process id up to 99999.
inline constexpr auto SharedObjectNameSuffixBits = 48;

/// @param processId The process making the object.
/// @param suffix What tells the name apart; only its low SharedObjectNameSuffixBits bits are used.
/// @return The name: `/contour-eh-<processId>-<suffix as 12 hex digits>`.
[[nodiscard]] inline std::string sharedObjectName(int64_t processId, uint64_t suffix)
{
    constexpr auto SuffixMask = (uint64_t { 1 } << SharedObjectNameSuffixBits) - 1;
    return std::format("/contour-eh-{}-{:012x}", processId, suffix & SuffixMask);
}

/// Creates an object under a fresh name: tries up to SharedObjectNameAttempts names, each with a new suffix,
/// for as long as the name tried is taken.
/// @param processId The process making the object.
/// @param nextSuffix Gives each attempt's suffix: a random value, never a constant.
/// @param create Tries one name: the object, or the `errno` it failed with; `EEXIST` if the name is taken.
/// @return What the first attempt not refused for a taken name gave; `EEXIST` if every name was taken.
template <typename NextSuffix, typename Create>
[[nodiscard]] auto createUnderFreshName(int64_t processId, NextSuffix const& nextSuffix, Create const& create)
    -> decltype(create(std::string {}))
{
    for ([[maybe_unused]] auto const attempt: std::views::iota(0, SharedObjectNameAttempts))
    {
        auto created = create(sharedObjectName(processId, nextSuffix()));
        if (created || created.error() != EEXIST)
            return created;
    }
    return decltype(create(std::string {})) { std::unexpect, EEXIST };
}

} // namespace vtextension::wasm::remote::transport
