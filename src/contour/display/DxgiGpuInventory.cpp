// SPDX-License-Identifier: Apache-2.0
#include <contour/display/DxgiGpuInventory.hpp>

#include <QtCore/QString>

#include <ranges>

#include <Windows.h>
#include <dxgi.h>

#include <wrl/client.h>

namespace contour::display
{

std::vector<GpuCandidate> DxgiGpuInventory::list() const
{
    auto factory = Microsoft::WRL::ComPtr<IDXGIFactory1> {};
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return {};

    auto gpus = std::vector<GpuCandidate> {};
    for (auto const index: std::views::iota(UINT { 0 }))
    {
        auto adapter = Microsoft::WRL::ComPtr<IDXGIAdapter1> {};
        if (FAILED(factory->EnumAdapters1(index, &adapter))) // DXGI_ERROR_NOT_FOUND past the last one
            break;
        auto description = DXGI_ADAPTER_DESC1 {};
        if (FAILED(adapter->GetDesc1(&description)) || (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
            continue;
        gpus.push_back(GpuCandidate {
            .title = QString::fromWCharArray(description.Description).toStdString(),
            .id = config::PciId { .vendor = static_cast<std::uint16_t>(description.VendorId),
                                  .device = static_cast<std::uint16_t>(description.DeviceId) },
            .kind = kindFromDedicatedVideoMemory(description.DedicatedVideoMemory),
            .output = GpuOutput::Offscreen,
            .driver = {},
        });
    }
    return gpus;
}

} // namespace contour::display
