// SPDX-License-Identifier: Apache-2.0
#include <contour/display/PciIds.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <memory>
#include <ranges>

namespace contour::display
{

namespace
{
    struct VendorShortName
    {
        std::uint16_t vendor;
        std::string_view name;
    };

    // The marketing names people know; pci.ids' vendor lines are legal entity names.
    constexpr auto VendorShortNames = std::array {
        VendorShortName { .vendor = 0x8086, .name = "Intel" },
        VendorShortName { .vendor = 0x10de, .name = "NVIDIA" },
        VendorShortName { .vendor = 0x1002, .name = "AMD" },
        VendorShortName { .vendor = 0x1414, .name = "Microsoft" },
        VendorShortName { .vendor = 0x15ad, .name = "VMware" },
        VendorShortName { .vendor = 0x5143, .name = "Qualcomm" },
        VendorShortName { .vendor = 0x106b, .name = "Apple" },
    };

    struct KindNoun
    {
        GpuKind kind;
        std::string_view noun;
    };

    constexpr auto KindNouns = std::array {
        KindNoun { .kind = GpuKind::Integrated, .noun = "integrated GPU" },
        KindNoun { .kind = GpuKind::Discrete, .noun = "discrete GPU" },
        KindNoun { .kind = GpuKind::Virtual, .noun = "virtual GPU" },
        KindNoun { .kind = GpuKind::Cpu, .noun = "software renderer" },
    };

    [[nodiscard]] std::optional<std::uint16_t> leadingHex16(std::string_view text) noexcept
    {
        if (text.size() < 4)
            return std::nullopt;
        auto value = std::uint16_t {};
        auto const* const first = std::to_address(text.begin());
        auto const [last, error] = std::from_chars(first, first + 4, value, 16);
        if (error != std::errc {} || last != first + 4 || (text.size() > 4 && text[4] != ' '))
            return std::nullopt;
        return value;
    }

    [[nodiscard]] std::string_view nameAfterId(std::string_view line) noexcept
    {
        auto const start = line.find_first_not_of(' ', 4);
        return start == std::string_view::npos ? std::string_view {} : line.substr(start);
    }

    /// The vendor's marketing name when known, else the database's, else a placeholder.
    [[nodiscard]] std::string vendorName(config::PciId id, std::optional<PciNames> const& names)
    {
        auto const shortName = std::ranges::find(VendorShortNames, id.vendor, &VendorShortName::vendor);
        if (shortName != VendorShortNames.end())
            return std::string(shortName->name);
        if (names)
            return names->vendor;
        return "Unknown vendor";
    }
} // namespace

std::optional<PciNames> lookupPciNames(std::string_view database, config::PciId id)
{
    auto result = std::optional<PciNames> {};
    for (auto const piece: database | std::views::split('\n'))
    {
        auto line = std::string_view(piece.begin(), piece.end());
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        if (line.empty() || line.front() == '#')
            continue;
        if (line.front() != '\t')
        {
            if (result)
                break; // the vendor's device block ended
            if (leadingHex16(line) == id.vendor)
                result = PciNames { .vendor = std::string(nameAfterId(line)), .device = std::nullopt };
            continue;
        }
        if (!result || line.starts_with("\t\t"))
            continue; // not our vendor, or a subsystem line
        auto const deviceLine = line.substr(1);
        if (leadingHex16(deviceLine) == id.device)
        {
            result->device = std::string(nameAfterId(deviceLine));
            break;
        }
    }
    return result;
}

std::string gpuTitle(config::PciId id, GpuKind kind, std::optional<PciNames> const& names)
{
    auto const vendor = vendorName(id, names);
    if (names && names->device)
    {
        auto const& device = *names->device;
        auto const open = device.find('[');
        auto const close = device.rfind(']');
        if (open != std::string::npos && close != std::string::npos && close > open)
            return std::format("{} {}", vendor, device.substr(open + 1, close - open - 1));
        return std::format("{} {}", vendor, device);
    }
    auto const noun = std::ranges::find(KindNouns, kind, &KindNoun::kind);
    return std::format("{} {}", vendor, noun != KindNouns.end() ? noun->noun : std::string_view { "GPU" });
}

} // namespace contour::display
