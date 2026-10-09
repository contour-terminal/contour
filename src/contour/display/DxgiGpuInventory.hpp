// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/display/GpuInventory.hpp>

#include <cstdint>

namespace contour::display
{

/// DXGI does not say whether an adapter is integrated. Integrated GPUs borrow system memory and report
/// little or no dedicated video memory; discrete ones report gigabytes.
/// @param bytes DXGI_ADAPTER_DESC1::DedicatedVideoMemory.
/// @return The kind to rank the adapter as.
[[nodiscard]] constexpr GpuKind kindFromDedicatedVideoMemory(std::uint64_t bytes) noexcept
{
    constexpr auto DiscreteThreshold = std::uint64_t { 512 } * 1024 * 1024;
    return bytes >= DiscreteThreshold ? GpuKind::Discrete : GpuKind::Integrated;
}

#if defined(_WIN32)
/// Lists GPUs through DXGI; titles are the adapters' own descriptions.
class DxgiGpuInventory final: public IGpuInventory
{
  public:
    [[nodiscard]] std::vector<GpuCandidate> list() const override;
};
#endif

} // namespace contour::display
