// SPDX-License-Identifier: Apache-2.0
#include <contour/display/GraphicsDeviceSelector.hpp>

#include <QtQuick/QQuickWindow>

#include <catch2/catch_test_macros.hpp>

#include <optional>

using namespace contour;
using namespace contour::display;

namespace
{
class FakeAdapterLister final: public IAdapterLister
{
  public:
    /// @param listings Counts list() calls.
    /// @param failing The adapter whose probe fails, if any.
    /// @param probes Counts probe() calls, if given.
    explicit FakeAdapterLister(int& listings,
                               std::optional<config::PciId> failing = std::nullopt,
                               int* probes = nullptr):
        _listings { listings }, _failing { failing }, _probes { probes }
    {
    }

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
    [[nodiscard]] AdapterProbe probe(AdapterEntry const& entry) override
    {
        if (_probes)
            ++*_probes;
        return entry.candidate.id == _failing ? AdapterProbe::Failed : AdapterProbe::Rendered;
    }
    [[nodiscard]] QVulkanInstance* vulkanInstance() noexcept override { return nullptr; }
    [[nodiscard]] std::string_view backendName() const noexcept override { return "Fake"; }

  private:
    int& _listings;
    std::optional<config::PciId> _failing;
    int* _probes;
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

TEST_CASE("GraphicsDeviceSelector: auto leaves the window to Qt and lists nothing", "[gpu]")
{
    auto listings = 0;
    auto selector =
        GraphicsDeviceSelector(std::make_unique<FakeAdapterLister>(listings), config::GpuSelector {});
    auto window = QQuickWindow {};
    selector.applyTo(window);
    CHECK(listings == 0);
}

TEST_CASE("GraphicsDeviceSelector: after a fallback, later windows still get the fallback GPU", "[gpu]")
{
    auto listings = 0;
    auto selector = GraphicsDeviceSelector(
        std::make_unique<FakeAdapterLister>(listings),
        config::GpuSelector { .preference = config::GpuPreference::Discrete, .id = std::nullopt });
    CHECK(selector.appliesDevice());
    (void) selector.fallBackToAuto();
    CHECK(selector.appliesDevice()); // the selector now reads auto, but the windows are on its choice

    auto untouched =
        GraphicsDeviceSelector(std::make_unique<FakeAdapterLister>(listings), config::GpuSelector {});
    CHECK_FALSE(untouched.appliesDevice());
}

TEST_CASE("GraphicsDeviceSelector: fallBackToAuto never re-chooses the GPU that failed", "[gpu]")
{
    // `integrated` failed on the Intel GPU; auto's ranking would pick it again.
    auto listings = 0;
    auto selector = GraphicsDeviceSelector(
        std::make_unique<FakeAdapterLister>(listings),
        config::GpuSelector { .preference = config::GpuPreference::Integrated, .id = std::nullopt });
    REQUIRE(selector.choose()->candidate.id == config::PciId { 0x8086, 0xa788 });
    auto const& fallback = selector.fallBackToAuto();
    REQUIRE(fallback.has_value());
    CHECK(fallback->candidate.id == config::PciId { 0x10de, 0x2820 });
}

TEST_CASE("GraphicsDeviceSelector: an explicit GPU that cannot render a probe frame is never handed out",
          "[gpu]")
{
    // The RTX under NVK loses its device on the first frame. Found before any window uses it, it never
    // reaches one: Qt leaks the window's surface lock when a frame fails (see the probe's comment).
    auto listings = 0;
    auto probes = 0;
    auto selector = GraphicsDeviceSelector(
        std::make_unique<FakeAdapterLister>(listings, config::PciId { 0x10de, 0x2820 }, &probes),
        config::GpuSelector { .preference = config::GpuPreference::Discrete, .id = std::nullopt });
    REQUIRE(selector.choose().has_value());
    CHECK(selector.choose()->candidate.id == config::PciId { 0x8086, 0xa788 });
    CHECK(selector.selector().preference == config::GpuPreference::Auto);
    CHECK(probes == 1);
    auto const fallback = selector.takeProbeFallback();
    REQUIRE(fallback.has_value());
    CHECK(fallback->failed == "NVIDIA GeForce RTX 4070 Laptop GPU");
    CHECK(fallback->used == "Intel(R) Graphics (RPL-S)");
    CHECK_FALSE(selector.takeProbeFallback().has_value()); // reported once
}

TEST_CASE("GraphicsDeviceSelector: an explicit GPU that renders its probe frame is kept", "[gpu]")
{
    auto listings = 0;
    auto probes = 0;
    auto selector = GraphicsDeviceSelector(
        std::make_unique<FakeAdapterLister>(listings, std::nullopt, &probes),
        config::GpuSelector { .preference = config::GpuPreference::Discrete, .id = std::nullopt });
    CHECK(selector.choose()->candidate.id == config::PciId { 0x10de, 0x2820 });
    (void) selector.choose();
    CHECK(probes == 1);
    CHECK_FALSE(selector.takeProbeFallback().has_value());
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
