// SPDX-License-Identifier: Apache-2.0
#include <contour/display/GraphicsDeviceSelector.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace contour;
using namespace contour::display;

namespace
{
class FakeAdapterLister final: public IAdapterLister
{
  public:
    explicit FakeAdapterLister(int& listings): _listings { listings } {}

    [[nodiscard]] std::vector<AdapterEntry> list() override
    {
        ++_listings;
        return {
            { .candidate = { .title = "NVIDIA GeForce RTX 4070 Laptop GPU",
                             .id = { 0x10de, 0x2820 },
                             .kind = GpuKind::Discrete,
                             .output = GpuOutput::Offscreen,
                             .driver = {} },
              .adapter = nullptr },
            { .candidate = { .title = "Intel(R) Graphics (RPL-S)",
                             .id = { 0x8086, 0xa788 },
                             .kind = GpuKind::Integrated,
                             .output = GpuOutput::Offscreen,
                             .driver = {} },
              .adapter = nullptr },
        };
    }
    [[nodiscard]] QVulkanInstance* vulkanInstance() noexcept override { return nullptr; }
    [[nodiscard]] std::string_view backendName() const noexcept override { return "Fake"; }

  private:
    int& _listings;
};
} // namespace

TEST_CASE("GraphicsDeviceSelector: lists once and chooses by the selector", "[gpu]")
{
    auto listings = 0;
    auto selector = GraphicsDeviceSelector(
        std::make_unique<FakeAdapterLister>(listings),
        config::GpuSelector { .preference = config::GpuPreference::Discrete, .id = std::nullopt });
    REQUIRE(selector.choose().has_value());
    CHECK(selector.choose()->candidate.id == config::PciId { 0x10de, 0x2820 });
    CHECK(listings == 1);
}

TEST_CASE("GraphicsDeviceSelector: fallBackToAuto re-chooses the power-saving GPU without listing again",
          "[gpu]")
{
    auto listings = 0;
    auto selector = GraphicsDeviceSelector(
        std::make_unique<FakeAdapterLister>(listings),
        config::GpuSelector { .preference = config::GpuPreference::Discrete, .id = std::nullopt });
    (void) selector.choose();
    auto const& fallback = selector.fallBackToAuto();
    REQUIRE(fallback.has_value());
    CHECK(fallback->candidate.id == config::PciId { 0x8086, 0xa788 });
    CHECK(selector.selector().preference == config::GpuPreference::Auto);
    CHECK(listings == 1);
}

TEST_CASE("adapterImplementationFor: only adapter-capable backends", "[gpu]")
{
    CHECK(adapterImplementationFor(config::RenderingBackend::Vulkan) == QRhi::Vulkan);
    CHECK(adapterImplementationFor(config::RenderingBackend::Direct3D11) == QRhi::D3D11);
    CHECK(adapterImplementationFor(config::RenderingBackend::Direct3D12) == QRhi::D3D12);
#if defined(_WIN32)
    CHECK(adapterImplementationFor(config::RenderingBackend::Auto) == QRhi::D3D11);
#else
    CHECK_FALSE(adapterImplementationFor(config::RenderingBackend::Auto).has_value());
#endif
    CHECK_FALSE(adapterImplementationFor(config::RenderingBackend::OpenGL).has_value());
    CHECK_FALSE(adapterImplementationFor(config::RenderingBackend::Software).has_value());
}

TEST_CASE("backendNameOf: names the adapter-capable backends for the startup log", "[gpu]")
{
    CHECK(backendNameOf(QRhi::Vulkan) == "Vulkan");
    CHECK(backendNameOf(QRhi::D3D11) == "Direct3D 11");
    CHECK(backendNameOf(QRhi::D3D12) == "Direct3D 12");
    CHECK(backendNameOf(QRhi::OpenGLES2) == "unknown");
}
