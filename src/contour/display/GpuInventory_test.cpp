// SPDX-License-Identifier: Apache-2.0
#include <contour/display/GpuInventory.hpp>
#include <contour/display/PciIds.hpp>
#include <contour/display/SysfsGpuInventory.hpp>

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <memory>

using namespace contour;
using namespace contour::display;

namespace
{
constexpr auto PciIdsText = std::string_view { "# comment\n"
                                               "8086  Intel Corporation\n"
                                               "\t0046  Core Processor Integrated Graphics Controller\n"
                                               "10de  NVIDIA Corporation\n"
                                               "\t2820  AD106M [GeForce RTX 4070 Max-Q / Mobile]\n"
                                               "\t\t1043 1234  Some subsystem\n"
                                               "1002  Advanced Micro Devices, Inc. [AMD/ATI]\n"
                                               "C 03  Display controller\n"
                                               "\t00  VGA compatible controller\n" };

/// An in-memory sysfs: files, symlinks and directory listings by path.
class FakeTextFileReader final: public ITextFileReader
{
  public:
    std::map<std::filesystem::path, std::string> files;
    std::map<std::filesystem::path, std::filesystem::path> links;
    std::map<std::filesystem::path, std::vector<std::string>> directories;

    [[nodiscard]] std::optional<std::string> read(std::filesystem::path const& path) const override
    {
        auto const found = files.find(path);
        return found != files.end() ? std::optional { found->second } : std::nullopt;
    }
    [[nodiscard]] std::vector<std::string> listDirectory(std::filesystem::path const& path) const override
    {
        auto const found = directories.find(path);
        return found != directories.end() ? found->second : std::vector<std::string> {};
    }
    [[nodiscard]] std::optional<std::filesystem::path> readLink(
        std::filesystem::path const& path) const override
    {
        auto const found = links.find(path);
        return found != links.end() ? std::optional { found->second } : std::nullopt;
    }

    void addCard(std::string const& card,
                 std::string const& address,
                 std::string const& vendor,
                 std::string const& device,
                 std::string const& bootVga,
                 std::string const& driver)
    {
        auto const dir = std::filesystem::path("/sys/class/drm") / card / "device";
        links[dir] = std::filesystem::path("../../../") / address;
        files[dir / "vendor"] = vendor + "\n";
        files[dir / "device"] = device + "\n";
        files[dir / "boot_vga"] = bootVga + "\n";
        links[dir / "driver"] = std::filesystem::path("../../../bus/pci/drivers") / driver;
    }
};

[[nodiscard]] std::shared_ptr<FakeTextFileReader> hybridLaptop()
{
    auto reader = std::make_shared<FakeTextFileReader>();
    reader->directories["/sys/class/drm"] = { "card0",      "card0-eDP-1", "card1",
                                              "card1-DP-1", "renderD128",  "renderD129",
                                              "version",    "card2",       "card3" };
    reader->addCard("card0", "0000:01:00.0", "0x10de", "0x2820", "0", "nouveau");
    reader->addCard("card1", "0000:00:02.0", "0x8086", "0xa788", "1", "i915");
    reader->addCard("card2", "0000:01:00.0", "0x10de", "0x2820", "0", "nouveau"); // same GPU, second node
    reader->links["/sys/class/drm/card3/device"] = "../../../platform-simple-framebuffer.0"; // not PCI
    reader->files["/usr/share/hwdata/pci.ids"] = std::string(PciIdsText);
    return reader;
}

[[nodiscard]] SysfsGpuInventory inventoryOver(std::shared_ptr<FakeTextFileReader const> reader)
{
    return SysfsGpuInventory(
        std::move(reader), "/sys/class/drm", { "/usr/share/hwdata/pci.ids", "/usr/share/misc/pci.ids" });
}
} // namespace

TEST_CASE("pci.ids: vendor-scoped device lookup", "[gpu]")
{
    auto const rtx = lookupPciNames(PciIdsText, { 0x10de, 0x2820 });
    REQUIRE(rtx.has_value());
    CHECK(rtx->vendor == "NVIDIA Corporation");
    CHECK(rtx->device == "AD106M [GeForce RTX 4070 Max-Q / Mobile]");

    auto const unknownDevice = lookupPciNames(PciIdsText, { 0x8086, 0xa788 });
    REQUIRE(unknownDevice.has_value());
    CHECK_FALSE(unknownDevice->device.has_value());

    // 0x2820 also appears as no other vendor's device here, and the class section must not match.
    CHECK_FALSE(lookupPciNames(PciIdsText, { 0x1234, 0x2820 }).has_value());
}

TEST_CASE("gpuTitle: human names, never raw ids", "[gpu]")
{
    CHECK(gpuTitle({ 0x10de, 0x2820 }, GpuKind::Discrete, lookupPciNames(PciIdsText, { 0x10de, 0x2820 }))
          == "NVIDIA GeForce RTX 4070 Max-Q / Mobile");
    CHECK(gpuTitle({ 0x8086, 0xa788 }, GpuKind::Integrated, lookupPciNames(PciIdsText, { 0x8086, 0xa788 }))
          == "Intel integrated GPU");
    CHECK(gpuTitle({ 0x8086, 0x0046 }, GpuKind::Integrated, lookupPciNames(PciIdsText, { 0x8086, 0x0046 }))
          == "Intel Core Processor Integrated Graphics Controller");
    CHECK(gpuTitle({ 0x1af4, 0x1050 }, GpuKind::Other, std::nullopt) == "Unknown vendor GPU");
}

TEST_CASE("SysfsGpuInventory: one candidate per GPU, connectors and non-PCI nodes skipped", "[gpu]")
{
    auto const gpus = inventoryOver(hybridLaptop()).list();
    REQUIRE(gpus.size() == 2);

    CHECK(gpus[0].title == "NVIDIA GeForce RTX 4070 Max-Q / Mobile");
    CHECK(gpus[0].id == config::PciId { 0x10de, 0x2820 });
    CHECK(gpus[0].kind == GpuKind::Discrete);
    CHECK(gpus[0].output == GpuOutput::Offscreen);
    CHECK(gpus[0].driver == "nouveau");

    CHECK(gpus[1].title == "Intel integrated GPU");
    CHECK(gpus[1].kind == GpuKind::Integrated);
    CHECK(gpus[1].output == GpuOutput::DrivesDisplay);
    CHECK(gpus[1].driver == "i915");
}

TEST_CASE("SysfsGpuInventory: without pci.ids, titles still avoid raw ids", "[gpu]")
{
    auto reader = hybridLaptop();
    reader->files.erase("/usr/share/hwdata/pci.ids");
    auto const gpus = inventoryOver(reader).list();
    REQUIRE(gpus.size() == 2);
    CHECK(gpus[0].title == "NVIDIA discrete GPU");
    CHECK(gpus[1].title == "Intel integrated GPU");
}
