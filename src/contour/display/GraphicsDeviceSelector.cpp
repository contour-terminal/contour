// SPDX-License-Identifier: Apache-2.0
#include <contour/Logging.hpp>
#include <contour/display/GraphicsDeviceSelector.hpp>
#include <contour/display/Logging.hpp>
#ifdef _WIN32
    #include <contour/display/GpuInventory.hpp>
#endif

#include <QtQuick/QQuickGraphicsDevice>
#include <QtQuick/QQuickWindow>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <memory>
#include <optional>
#include <ranges>
#include <type_traits>

// Qt declares its Vulkan API (QVulkanInstance, QRhiVulkanInitParams, QWindow::setVulkanInstance) only for
// a Qt built with Vulkan, and only where the Vulkan headers are installed; Direct3D needs neither.
#if QT_CONFIG(vulkan) && __has_include(<vulkan/vulkan.h>)
    #define CONTOUR_GPU_SELECTION_VULKAN 1
    #include <QtGui/QVulkanInstance>
#endif

namespace contour::display
{

namespace
{
    [[nodiscard]] GpuKind kindOf(QRhiDriverInfo::DeviceType type) noexcept
    {
        switch (type)
        {
            case QRhiDriverInfo::IntegratedDevice: return GpuKind::Integrated;
            case QRhiDriverInfo::DiscreteDevice:
            case QRhiDriverInfo::ExternalDevice: return GpuKind::Discrete;
            case QRhiDriverInfo::VirtualDevice: return GpuKind::Virtual;
            case QRhiDriverInfo::CpuDevice: return GpuKind::Cpu;
            case QRhiDriverInfo::UnknownDevice: break;
        }
        return GpuKind::Other;
    }

    struct BackendName
    {
        QRhi::Implementation implementation;
        std::string_view name;
    };

    constexpr auto BackendNames = std::array {
        BackendName { .implementation = QRhi::Vulkan, .name = "Vulkan" },
        BackendName { .implementation = QRhi::D3D11, .name = "Direct3D 11" },
        BackendName { .implementation = QRhi::D3D12, .name = "Direct3D 12" },
    };

    /// Clears a one-pixel texture on @p rhi's device and reads it back.
    ///
    /// Done offscreen, on a device of its own, before any window sees the adapter: a window's first frame
    /// is the wrong place to find out. When a frame fails there, QRhiVulkan::beginFrame() has already
    /// taken the window's platform frame lock (QPlatformVulkanInstance::beginFrame, Wayland's surface lock)
    /// and returns without releasing it, so the window can never be destroyed again (qrhivulkan.cpp,
    /// Qt 6.10: beginFrame() returns on waitCommandCompletion()'s device loss before endFrame()'s
    /// cleanup is armed). An offscreen frame takes no window lock.
    [[nodiscard]] AdapterProbe renderProbeFrame(QRhi& rhi)
    {
        auto const texture = std::unique_ptr<QRhiTexture>(
            rhi.newTexture(QRhiTexture::RGBA8,
                           QSize(1, 1),
                           1,
                           QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
        if (!texture->create())
            return AdapterProbe::Failed;
        auto const target =
            std::unique_ptr<QRhiTextureRenderTarget>(rhi.newTextureRenderTarget({ texture.get() }));
        auto const pass =
            std::unique_ptr<QRhiRenderPassDescriptor>(target->newCompatibleRenderPassDescriptor());
        target->setRenderPassDescriptor(pass.get());
        if (!target->create())
            return AdapterProbe::Failed;

        QRhiCommandBuffer* commands = nullptr;
        if (rhi.beginOffscreenFrame(&commands) != QRhi::FrameOpSuccess)
            return AdapterProbe::Failed;
        auto readback = QRhiReadbackResult {};
        auto* const updates = rhi.nextResourceUpdateBatch();
        commands->beginPass(target.get(), QColor::fromRgbF(1.0f, 0.5f, 0.25f, 1.0f), { 1.0f, 0 });
        updates->readBackTexture(QRhiReadbackDescription { texture.get() }, &readback);
        commands->endPass(updates);
        if (rhi.endOffscreenFrame() != QRhi::FrameOpSuccess || rhi.isDeviceLost())
            return AdapterProbe::Failed;

        // A lost device can also just leave the pixel unwritten.
        constexpr auto Expected = std::array<int, 4> { 255, 128, 64, 255 };
        if (readback.data.size() < 4)
            return AdapterProbe::Failed;
        for (auto const channel: std::views::iota(0, 4))
            if (std::abs(static_cast<unsigned char>(readback.data[channel])
                         - Expected[static_cast<std::size_t>(channel)])
                > 2)
                return AdapterProbe::Failed;
        return AdapterProbe::Rendered;
    }

    /// What QtAdapterLister::withInitParams() returns for a @p Use returning R: R itself when it is a
    /// pointer (null meaning "no backend"), else std::optional<R>.
    template <typename Use>
    using InitParamsResult = std::conditional_t<std::is_pointer_v<std::invoke_result_t<Use, QRhiInitParams*>>,
                                                std::invoke_result_t<Use, QRhiInitParams*>,
                                                std::optional<std::invoke_result_t<Use, QRhiInitParams*>>>;

    class QtAdapterLister final: public IAdapterLister
    {
      public:
        explicit QtAdapterLister(QRhi::Implementation implementation): _implementation { implementation }
        {
#ifdef CONTOUR_GPU_SELECTION_VULKAN
            if (implementation != QRhi::Vulkan)
                return;
            // What Qt Quick's default instance asks for, so adopting this one changes nothing else.
            _vulkan = std::make_unique<QVulkanInstance>();
            _vulkan->setApiVersion(_vulkan->supportedApiVersion());
            _vulkan->setExtensions(QRhiVulkanInitParams::preferredInstanceExtensions());
            if (!_vulkan->create())
            {
                errorLog()("GPU selection: creating the Vulkan instance failed ({}).",
                           static_cast<int>(_vulkan->errorCode()));
                _vulkan.reset();
            }
#endif
        }

        ~QtAdapterLister() override { qDeleteAll(_owned); }

        QtAdapterLister(QtAdapterLister const&) = delete;
        QtAdapterLister& operator=(QtAdapterLister const&) = delete;
        QtAdapterLister(QtAdapterLister&&) = delete;
        QtAdapterLister& operator=(QtAdapterLister&&) = delete;

        [[nodiscard]] std::vector<AdapterEntry> list() override
        {
            qDeleteAll(_owned);
            _owned = listAdapters();
            auto entries = std::vector<AdapterEntry> {};
            for (auto* const adapter: _owned)
            {
                auto const info = adapter->info();
                entries.push_back(AdapterEntry {
                    .candidate =
                        GpuCandidate {
                            .title = info.deviceName.toStdString(),
                            .id = config::PciId { .vendor = static_cast<std::uint16_t>(info.vendorId),
                                                  .device = static_cast<std::uint16_t>(info.deviceId) },
                            .kind = kindOf(info.deviceType),
                            .output = GpuOutput::Offscreen,
                            .driver = {} },
                    .adapter = adapter });
            }
#ifdef _WIN32
            // Qt's Direct3D backends report every hardware adapter as UnknownDevice (QRhiD3D::fillDriverInfo
            // knows only "software or not"), so integrated/discrete could never match. DXGI's own listing
            // classifies them; match it by vendor:device.
            if (_implementation == QRhi::D3D11 || _implementation == QRhi::D3D12)
            {
                auto const inventory = makePlatformGpuInventory()->list();
                for (auto& entry: entries)
                    entry.candidate.kind = inventoryKindOf(entry.candidate, inventory);
            }
#endif
            return entries;
        }

        [[nodiscard]] AdapterProbe probe(AdapterEntry const& entry) override
        {
            auto const rhi = std::unique_ptr<QRhi>(withInitParams([&](QRhiInitParams* params) {
                return QRhi::create(_implementation, params, {}, nullptr, entry.adapter);
            }));
            return rhi ? renderProbeFrame(*rhi) : AdapterProbe::Failed;
        }

        [[nodiscard]] QVulkanInstance* vulkanInstance() noexcept override
        {
#ifdef CONTOUR_GPU_SELECTION_VULKAN
            return _vulkan.get();
#else
            return nullptr;
#endif
        }

        [[nodiscard]] std::string_view backendName() const noexcept override
        {
            return backendNameOf(_implementation);
        }

      private:
        [[nodiscard]] QRhi::AdapterList listAdapters()
        {
            auto adapters = withInitParams(
                [&](QRhiInitParams* params) { return QRhi::enumerateAdapters(_implementation, params); });
            return adapters.value_or(QRhi::AdapterList {});
        }

        /// Calls @p use with the init parameters every QRhi of this backend is made with.
        /// @return What @p use returned; nullopt (or nullptr) when the backend cannot be initialized here.
        template <typename Use>
        [[nodiscard]] InitParamsResult<Use> withInitParams([[maybe_unused]] Use&& use)
        {
            using Optional = InitParamsResult<Use>;
            switch (_implementation)
            {
#ifdef CONTOUR_GPU_SELECTION_VULKAN
                case QRhi::Vulkan: {
                    if (!_vulkan)
                        return Optional {};
                    auto params = QRhiVulkanInitParams {};
                    params.inst = _vulkan.get();
                    return Optional { use(&params) };
                }
#endif
#ifdef _WIN32
                case QRhi::D3D11: {
                    auto params = QRhiD3D11InitParams {};
                    return Optional { use(&params) };
                }
                case QRhi::D3D12: {
                    auto params = QRhiD3D12InitParams {};
                    return Optional { use(&params) };
                }
#endif
                default: return Optional {};
            }
        }

        QRhi::Implementation _implementation;
#ifdef CONTOUR_GPU_SELECTION_VULKAN
        std::unique_ptr<QVulkanInstance> _vulkan;
#endif
        QRhi::AdapterList _owned;
    };
} // namespace

GraphicsDeviceSelector::GraphicsDeviceSelector(std::unique_ptr<IAdapterLister> lister,
                                               config::GpuSelector selector):
    _lister { std::move(lister) }, _selector { selector }
{
}

std::optional<AdapterEntry> const& GraphicsDeviceSelector::choose()
{
    if (!_adapters)
    {
        _adapters = _lister->list();
        recompute(std::nullopt);
        probeExplicitChoice();
    }
    return _chosen;
}

void GraphicsDeviceSelector::probeExplicitChoice()
{
    // `auto` is what Qt would choose anyway; only a GPU the user asked for is put to the test.
    if (_selector.preference == config::GpuPreference::Auto || !_chosen
        || _lister->probe(*_chosen) == AdapterProbe::Rendered)
        return;
    auto failed = _chosen->candidate.title;
    (void) fallBackToAuto();
    if (!_chosen)
    {
        errorLog()("{} could not render, and there is no other GPU to fall back to.", failed);
        return;
    }
    _probeFallback = GpuFallback { .failed = std::move(failed), .used = _chosen->candidate.title };
}

void GraphicsDeviceSelector::recompute(std::optional<config::PciId> excluded)
{
    // The candidates that may be chosen, and where each sits in the adapter list.
    auto candidates = std::vector<GpuCandidate> {};
    auto positions = std::vector<std::size_t> {};
    for (auto const index: std::views::iota(std::size_t { 0 }, _adapters->size()))
    {
        if (excluded && (*_adapters)[index].candidate.id == *excluded)
            continue;
        candidates.push_back((*_adapters)[index].candidate);
        positions.push_back(index);
    }
    auto const choice = chooseGpu(candidates, _selector);
    if (!choice)
    {
        _chosen.reset();
        return;
    }
    _chosen = (*_adapters)[positions[choice->index]];
    if (choice->outcome == RequestOutcome::FellBack)
        errorLog()("renderer.gpu: no GPU {} is present; using {}.", _selector, _chosen->candidate.title);
    // Only what was asked of the graphics API; the GPU it really runs on is logged once the scene graph is
    // up (see ContourGuiApp::applyGraphicsDevice).
    displayLog()("renderer.gpu: asking {} for '{}' {} (requested: {})",
                 _lister->backendName(),
                 _chosen->candidate.title,
                 _chosen->candidate.id,
                 _selector);
}

void GraphicsDeviceSelector::applyTo(QQuickWindow& window)
{
    // `auto` does not intervene: no adapter listing, no instance of our own, Qt's default device.
    if (!appliesDevice())
        return;
    auto const& chosen = choose();
#ifdef CONTOUR_GPU_SELECTION_VULKAN
    if (auto* const instance = _lister->vulkanInstance())
        window.setVulkanInstance(instance);
#endif
    if (chosen && chosen->adapter)
        window.setGraphicsDevice(QQuickGraphicsDevice::fromRhiAdapter(chosen->adapter));
}

std::optional<AdapterEntry> const& GraphicsDeviceSelector::fallBackToAuto()
{
    auto const failed = choose();
    _selector = config::GpuSelector {};
    recompute(failed ? std::optional { failed->candidate.id } : std::nullopt);
    return _chosen;
}

std::optional<QRhi::Implementation> adapterImplementationFor(config::RenderingBackend backend)
{
    switch (backend)
    {
        case config::RenderingBackend::Vulkan: return QRhi::Vulkan;
        case config::RenderingBackend::Direct3D11: return QRhi::D3D11;
        case config::RenderingBackend::Direct3D12: return QRhi::D3D12;
#ifdef _WIN32
        case config::RenderingBackend::Auto: return QRhi::D3D11; // Qt's default on Windows
#endif
        default: return std::nullopt;
    }
}

std::string_view backendNameOf(QRhi::Implementation implementation) noexcept
{
    auto const it = std::ranges::find(BackendNames, implementation, &BackendName::implementation);
    return it != BackendNames.end() ? it->name : std::string_view { "unknown" };
}

std::unique_ptr<IAdapterLister> makeQtAdapterLister(QRhi::Implementation implementation)
{
    return std::make_unique<QtAdapterLister>(implementation);
}

} // namespace contour::display
