// SPDX-License-Identifier: Apache-2.0
#include <contour/display/PciIds.hpp>
#include <contour/display/SysfsGpuInventory.hpp>

#include <core/Utils.hpp>

#include <algorithm>
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
    auto gpus = std::vector<GpuCandidate> {};
    for (auto const& name: names)
    {
        if (!isCardNode(name))
            continue;
        auto const device = _drmRoot / name / "device";
        auto const link = _reader->readLink(device);
        if (!link)
            continue;
        auto const address = link->filename().string();
        if (!isPciAddress(address) || seenAddresses.contains(address))
            continue;
        auto const vendor = sysfsHex(_reader->read(device / "vendor"));
        auto const deviceId = sysfsHex(_reader->read(device / "device"));
        if (!vendor || !deviceId)
            continue;
        seenAddresses.insert(address); // only a readable card claims its address

        auto const id = config::PciId { .vendor = *vendor, .device = *deviceId };
        // Integrated GPUs sit on the root bus; discrete ones behind a PCIe bridge.
        auto const kind = address.substr(5, 2) == "00" ? GpuKind::Integrated : GpuKind::Discrete;
        auto const bootVga = _reader->read(device / "boot_vga");
        auto const driverLink = _reader->readLink(device / "driver");
        gpus.push_back(GpuCandidate {
            .title = gpuTitle(id, kind, database ? lookupPciNames(*database, id) : std::nullopt),
            .id = id,
            .kind = kind,
            .output =
                bootVga && core::trim(*bootVga) == "1" ? GpuOutput::DrivesDisplay : GpuOutput::Offscreen,
            .driver = driverLink ? driverLink->filename().string() : std::string {},
        });
    }
    return gpus;
}

} // namespace contour::display
