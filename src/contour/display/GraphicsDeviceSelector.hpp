// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/Config.hpp>
#include <contour/config/GpuSelector.hpp>
#include <contour/display/GpuSelection.hpp>

#include <QtGui/rhi/qrhi.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

class QQuickWindow;
class QVulkanInstance;

namespace contour::display
{

/// One adapter a QRhi backend offers.
struct AdapterEntry
{
    GpuCandidate candidate;         ///< What the adapter is.
    QRhiAdapter* adapter = nullptr; ///< Owned by the IAdapterLister that listed it.
};

/// Whether an adapter rendered a probe frame.
enum class AdapterProbe : std::uint8_t
{
    Failed,   ///< The device was lost, or the frame did not produce the expected pixel.
    Rendered, ///< It rendered.
};

/// Lists a backend's adapters; injected so the choice is testable without a GPU.
class IAdapterLister
{
  public:
    virtual ~IAdapterLister() = default;

    /// Lists the adapters afresh, releasing those of the previous call.
    /// @return The adapters; their pointers stay valid until the next list() call, or the lister's end.
    [[nodiscard]] virtual std::vector<AdapterEntry> list() = 0;
    /// Renders one offscreen frame on @p entry's adapter, through a device of its own.
    /// @param entry An adapter from the latest list().
    /// @return Whether the frame rendered.
    [[nodiscard]] virtual AdapterProbe probe(AdapterEntry const& entry) = 0;
    /// @return The Vulkan instance every window must use, or nullptr for other backends.
    [[nodiscard]] virtual QVulkanInstance* vulkanInstance() noexcept = 0;
    /// @return The human-readable name of the backend whose adapters this lists, for the startup log.
    [[nodiscard]] virtual std::string_view backendName() const noexcept = 0;
};

/// Applies `renderer.gpu` to Vulkan and Direct3D windows through Qt's adapter API.
///
/// The adapters are listed once, on the first window, on the GUI thread; for Vulkan through the same
/// instance the windows use, so the driver probes the GPUs once per process (as Qt alone would).
class GraphicsDeviceSelector
{
  public:
    /// @param lister The backend's adapters.
    /// @param selector What `renderer.gpu` asks for.
    GraphicsDeviceSelector(std::unique_ptr<IAdapterLister> lister, config::GpuSelector selector);

    /// Lists the adapters on first use and chooses one. An explicitly chosen adapter must first render a
    /// probe frame; if it cannot, `auto`'s ranking chooses among the others (see takeProbeFallback()).
    /// @return The chosen adapter, or nullopt when the backend offers none.
    [[nodiscard]] std::optional<AdapterEntry> const& choose();

    /// Hands the instance and the chosen adapter to @p window. Must run before it is first exposed.
    /// Does nothing for `auto`, which leaves the device to Qt.
    void applyTo(QQuickWindow& window);

    /// Chooses again by `auto`'s ranking among the other adapters, after the chosen GPU failed to render.
    /// @return The new choice; nullopt when no other adapter exists.
    std::optional<AdapterEntry> const& fallBackToAuto();

    /// @return Whether applyTo() hands windows a device: true once a GPU was chosen explicitly, also after
    /// a fallback to `auto`'s ranking; false for `auto` from the start, which leaves the device to Qt.
    [[nodiscard]] bool appliesDevice() const noexcept
    {
        return _adapters.has_value() || _selector.preference != config::GpuPreference::Auto;
    }

    /// @return The fallback choose() made because the requested GPU failed its probe frame, once; then
    /// nullopt.
    [[nodiscard]] std::optional<GpuFallback> takeProbeFallback() noexcept
    {
        return std::exchange(_probeFallback, {});
    }

    /// @return The selector currently in effect.
    [[nodiscard]] config::GpuSelector const& selector() const noexcept { return _selector; }

  private:
    /// Chooses among the listed adapters by the current selector.
    /// @param excluded An adapter id that must not be chosen (one that failed to render).
    void recompute(std::optional<config::PciId> excluded);

    /// Falls back to `auto`'s ranking when the explicitly chosen adapter cannot render a probe frame.
    void probeExplicitChoice();

    std::unique_ptr<IAdapterLister> _lister;
    config::GpuSelector _selector;
    std::optional<std::vector<AdapterEntry>> _adapters; ///< nullopt until listed.
    std::optional<AdapterEntry> _chosen;
    std::optional<GpuFallback> _probeFallback;
};

/// @return The QRhi implementation whose adapters `renderer.gpu` chooses for @p backend, if any.
[[nodiscard]] std::optional<QRhi::Implementation> adapterImplementationFor(config::RenderingBackend backend);

/// @return The human-readable name of @p implementation as shown in the startup log, e.g. "Vulkan";
/// "unknown" for a backend this selector does not handle.
[[nodiscard]] std::string_view backendNameOf(QRhi::Implementation implementation) noexcept;

/// @return The lister over QRhi::enumerateAdapters; for Vulkan it creates and owns the instance.
[[nodiscard]] std::unique_ptr<IAdapterLister> makeQtAdapterLister(QRhi::Implementation implementation);

} // namespace contour::display
