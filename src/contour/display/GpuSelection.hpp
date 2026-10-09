// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/GpuSelector.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <string>

namespace contour::display
{

/// What kind of device a GPU is, as far as choosing one is concerned.
enum class GpuKind : std::uint8_t
{
    Other,
    Integrated,
    Discrete,
    Virtual,
    Cpu,
};

/// Whether a GPU drives the display (on Linux: sysfs `boot_vga`).
enum class GpuOutput : std::uint8_t
{
    Offscreen,
    DrivesDisplay,
};

/// One GPU, as an inventory or a graphics API lists it.
struct GpuCandidate
{
    std::string title;                       ///< Human-readable name, shown in the settings page.
    config::PciId id;                        ///< PCI vendor:device.
    GpuKind kind = GpuKind::Other;           ///< Integrated, discrete, ...
    GpuOutput output = GpuOutput::Offscreen; ///< Whether it drives the display.
    std::string driver;                      ///< Kernel driver name (Linux), empty when unknown.
};

/// Whether the choice is what was asked for.
enum class RequestOutcome : std::uint8_t
{
    Satisfied, ///< The request was met as stated.
    FellBack,  ///< A specific GPU was asked for but is absent; the Auto choice was taken instead.
};

/// The result of chooseGpu().
struct GpuChoice
{
    std::size_t index = 0;                              ///< Index into the candidates.
    RequestOutcome outcome = RequestOutcome::Satisfied; ///< Whether the request was met.
};

namespace detail
{
    /// Order in which kinds are tried, per preference. The CPU rasterizer is everyone's last resort.
    struct KindRanking
    {
        config::GpuPreference preference;
        std::array<GpuKind, 5> order;
    };

    inline constexpr auto KindRankingTable = std::array {
        KindRanking { .preference = config::GpuPreference::Auto,
                      .order = { GpuKind::Integrated,
                                 GpuKind::Discrete,
                                 GpuKind::Virtual,
                                 GpuKind::Other,
                                 GpuKind::Cpu } },
        KindRanking { .preference = config::GpuPreference::Integrated,
                      .order = { GpuKind::Integrated,
                                 GpuKind::Discrete,
                                 GpuKind::Virtual,
                                 GpuKind::Other,
                                 GpuKind::Cpu } },
        KindRanking { .preference = config::GpuPreference::Discrete,
                      .order = { GpuKind::Discrete,
                                 GpuKind::Integrated,
                                 GpuKind::Virtual,
                                 GpuKind::Other,
                                 GpuKind::Cpu } },
    };

    [[nodiscard]] inline std::optional<std::size_t> firstByRanking(std::span<GpuCandidate const> candidates,
                                                                   config::GpuPreference preference)
    {
        auto const row = std::ranges::find(KindRankingTable, preference, &KindRanking::preference);
        if (row == KindRankingTable.end())
            return std::nullopt;
        for (auto const kind: row->order)
            for (auto const index: std::views::iota(std::size_t { 0 }, candidates.size()))
                if (candidates[index].kind == kind)
                    return index;
        return std::nullopt;
    }
} // namespace detail

/// The kind of @p candidate, completed from an inventory: some graphics APIs cannot tell what an adapter
/// is (Qt's Direct3D backends report every hardware adapter as of unknown type), the inventory can.
/// @param candidate The adapter as the graphics API listed it.
/// @param inventory The machine's GPUs, as the platform inventory lists them.
/// @return The candidate's own kind when it knows one; else the kind the inventory gives the same
///         vendor:device; else GpuKind::Other.
[[nodiscard]] inline GpuKind inventoryKindOf(GpuCandidate const& candidate,
                                             std::span<GpuCandidate const> inventory)
{
    if (candidate.kind != GpuKind::Other)
        return candidate.kind;
    auto const match = std::ranges::find(inventory, candidate.id, &GpuCandidate::id);
    return match != inventory.end() ? match->kind : GpuKind::Other;
}

/// Chooses the GPU to render with.
/// @param candidates The GPUs, in the order they were listed.
/// @param selector What the configuration asks for.
/// @return The choice, or nullopt when there are no candidates.
[[nodiscard]] inline std::optional<GpuChoice> chooseGpu(std::span<GpuCandidate const> candidates,
                                                        config::GpuSelector const& selector)
{
    if (candidates.empty())
        return std::nullopt;

    if (selector.preference == config::GpuPreference::Specific)
    {
        if (selector.id)
        {
            auto const match = std::ranges::find(candidates, *selector.id, &GpuCandidate::id);
            if (match != candidates.end())
                return GpuChoice { .index = static_cast<std::size_t>(match - candidates.begin()),
                                   .outcome = RequestOutcome::Satisfied };
        }
        auto const fallback = detail::firstByRanking(candidates, config::GpuPreference::Auto);
        return GpuChoice { .index = fallback.value_or(0), .outcome = RequestOutcome::FellBack };
    }

    auto const index = detail::firstByRanking(candidates, selector.preference);
    return GpuChoice { .index = index.value_or(0), .outcome = RequestOutcome::Satisfied };
}

} // namespace contour::display
