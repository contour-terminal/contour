// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/Config.hpp>
#include <contour/config/GpuSelector.hpp>
#include <contour/display/GpuSelection.hpp>

#include <QtGui/rhi/qrhi.h>

#include <memory>
#include <optional>
#include <string_view>
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

/// Lists a backend's adapters; injected so the choice is testable without a GPU.
class IAdapterLister
{
  public:
    virtual ~IAdapterLister() = default;

    /// @return The adapters; their pointers stay valid for the lister's lifetime.
    [[nodiscard]] virtual std::vector<AdapterEntry> list() = 0;
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

    /// Lists the adapters on first use and chooses one.
    /// @return The chosen adapter, or nullopt when the backend offers none.
    [[nodiscard]] std::optional<AdapterEntry> const& choose();

    /// Hands the instance and the chosen adapter to @p window. Must run before it is first exposed.
    void applyTo(QQuickWindow& window);

    /// Chooses again as `auto`, after the chosen GPU failed to render.
    /// @return The new choice.
    std::optional<AdapterEntry> const& fallBackToAuto();

    /// @return The selector currently in effect.
    [[nodiscard]] config::GpuSelector const& selector() const noexcept { return _selector; }

  private:
    void recompute();

    std::unique_ptr<IAdapterLister> _lister;
    config::GpuSelector _selector;
    std::optional<std::vector<AdapterEntry>> _adapters; ///< nullopt until listed.
    std::optional<AdapterEntry> _chosen;
};

/// @return The QRhi implementation whose adapters `renderer.gpu` chooses for @p backend, if any.
[[nodiscard]] std::optional<QRhi::Implementation> adapterImplementationFor(config::RenderingBackend backend);

/// @return The human-readable name of @p implementation as shown in the startup log, e.g. "Vulkan";
/// "unknown" for a backend this selector does not handle.
[[nodiscard]] std::string_view backendNameOf(QRhi::Implementation implementation) noexcept;

/// @return The lister over QRhi::enumerateAdapters; for Vulkan it creates and owns the instance.
[[nodiscard]] std::unique_ptr<IAdapterLister> makeQtAdapterLister(QRhi::Implementation implementation);

} // namespace contour::display
