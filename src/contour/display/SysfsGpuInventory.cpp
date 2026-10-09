// SPDX-License-Identifier: Apache-2.0
#include <contour/display/PciIds.hpp>
#include <contour/display/SysfsGpuInventory.hpp>

#include <core/Utils.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <set>

namespace contour::display
{

namespace
{
    /// "card" followed by digits only; "card1-eDP-1" is a connector, not a GPU.
    [[nodiscard]] bool isCardNode(std::string_view name) noexcept
    {
        return name.size() > 4 && name.starts_with("card")
               && std::ranges::all_of(name.substr(4), [](char c) { return c >= '0' && c <= '9'; });
    }

    /// "dddd:bb:dd.f"
    [[nodiscard]] bool isPciAddress(std::string_view address) noexcept
    {
        return address.size() == 12 && address[4] == ':' && address[7] == ':' && address[10] == '.';
    }

    [[nodiscard]] std::optional<std::uint16_t> sysfsHex(std::optional<std::string> const& text) noexcept
    {
        if (!text)
            return std::nullopt;
        auto value = core::trim(*text);
        if (value.starts_with("0x"))
            value.remove_prefix(2);
        auto number = std::uint16_t {};
        auto const [last, error] = std::from_chars(value.data(), value.data() + value.size(), number, 16);
        if (error != std::errc {} || last != value.data() + value.size())
            return std::nullopt;
        return number;
    }

    /// Vendors that build GPUs into their CPUs. Only their display GPU can be integrated off bus 0.
    constexpr auto IntegratedGpuVendors = std::array<std::uint16_t, 2> {
        0x8086, // Intel
        0x1002, // AMD
    };

    /// One GPU as sysfs describes it, before it is classified.
    struct SysfsGpu
    {
        std::string address;
        config::PciId id;
        GpuOutput output = GpuOutput::Offscreen;
        std::string driver;
    };

    /// Integrated GPUs usually sit on the root bus and discrete ones behind a PCIe bridge. AMD APUs are
    /// the exception: their iGPU sits behind an internal bridge too. On a machine with more than one GPU,
    /// the one an Intel or AMD CPU drives the display with is taken to be that CPU's iGPU.
    [[nodiscard]] GpuKind kindOf(SysfsGpu const& gpu, std::size_t gpuCount)
    {
        if (gpu.address.substr(5, 2) == "00")
            return GpuKind::Integrated;
        if (gpuCount >= 2 && gpu.output == GpuOutput::DrivesDisplay
            && std::ranges::find(IntegratedGpuVendors, gpu.id.vendor) != IntegratedGpuVendors.end())
            return GpuKind::Integrated;
        return GpuKind::Discrete;
    }
} // namespace

SysfsGpuInventory::SysfsGpuInventory(std::shared_ptr<ITextFileReader const> reader,
                                     std::filesystem::path drmRoot,
                                     std::vector<std::filesystem::path> pciIdsPaths):
    _reader { std::move(reader) }, _drmRoot { std::move(drmRoot) }, _pciIdsPaths { std::move(pciIdsPaths) }
{
}

std::vector<GpuCandidate> SysfsGpuInventory::list() const
{
    auto database = std::optional<std::string> {};
    for (auto const& path: _pciIdsPaths)
    {
        database = _reader->read(path);
        if (database)
            break;
    }

    auto names = _reader->listDirectory(_drmRoot);
    std::ranges::sort(names); // directory order is unspecified; the dropdown order must not be

    auto seenAddresses = std::set<std::string> {};
    auto found = std::vector<SysfsGpu> {};
    for (auto const& name: names)
    {
        if (!isCardNode(name))
            continue;
        auto const device = _drmRoot / name / "device";
        auto const link = _reader->readLink(device);
        if (!link)
            continue;
        auto address = link->filename().string();
        if (!isPciAddress(address) || seenAddresses.contains(address))
            continue;
        auto const vendor = sysfsHex(_reader->read(device / "vendor"));
        auto const deviceId = sysfsHex(_reader->read(device / "device"));
        if (!vendor || !deviceId)
            continue;
        seenAddresses.insert(address); // only a readable card claims its address

        auto const bootVga = _reader->read(device / "boot_vga");
        auto const driverLink = _reader->readLink(device / "driver");
        found.push_back(SysfsGpu {
            .address = std::move(address),
            .id = config::PciId { .vendor = *vendor, .device = *deviceId },
            .output =
                bootVga && core::trim(*bootVga) == "1" ? GpuOutput::DrivesDisplay : GpuOutput::Offscreen,
            .driver = driverLink ? driverLink->filename().string() : std::string {},
        });
    }

    // Classified only once all are known: whether a GPU is integrated depends on whether it is alone.
    auto gpus = std::vector<GpuCandidate> {};
    for (auto& gpu: found)
    {
        auto const kind = kindOf(gpu, found.size());
        gpus.push_back(GpuCandidate {
            .title = gpuTitle(gpu.id, kind, database ? lookupPciNames(*database, gpu.id) : std::nullopt),
            .id = gpu.id,
            .kind = kind,
            .output = gpu.output,
            .driver = std::move(gpu.driver),
        });
    }
    return gpus;
}

} // namespace contour::display
