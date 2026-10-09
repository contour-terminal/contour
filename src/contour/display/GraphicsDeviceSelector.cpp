// SPDX-License-Identifier: Apache-2.0
#include <contour/Logging.hpp>
#include <contour/display/GraphicsDeviceSelector.hpp>

#include <QtGui/QVulkanInstance>
#include <QtQuick/QQuickGraphicsDevice>
#include <QtQuick/QQuickWindow>

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

    class QtAdapterLister final: public IAdapterLister
    {
      public:
        explicit QtAdapterLister(QRhi::Implementation implementation): _implementation { implementation }
        {
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
            return entries;
        }

        [[nodiscard]] QVulkanInstance* vulkanInstance() noexcept override { return _vulkan.get(); }

      private:
        [[nodiscard]] QRhi::AdapterList listAdapters()
        {
            switch (_implementation)
            {
                case QRhi::Vulkan: {
                    if (!_vulkan)
                        return {};
                    auto params = QRhiVulkanInitParams {};
                    params.inst = _vulkan.get();
                    return QRhi::enumerateAdapters(_implementation, &params);
                }
#if defined(_WIN32)
                case QRhi::D3D11: {
                    auto params = QRhiD3D11InitParams {};
                    return QRhi::enumerateAdapters(_implementation, &params);
                }
                case QRhi::D3D12: {
                    auto params = QRhiD3D12InitParams {};
                    return QRhi::enumerateAdapters(_implementation, &params);
                }
#endif
                default: return {};
            }
        }

        QRhi::Implementation _implementation;
        std::unique_ptr<QVulkanInstance> _vulkan;
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
        recompute();
    }
    return _chosen;
}

void GraphicsDeviceSelector::recompute()
{
    auto candidates = std::vector<GpuCandidate> {};
    for (auto const& entry: *_adapters)
        candidates.push_back(entry.candidate);
    auto const choice = chooseGpu(candidates, _selector);
    if (!choice)
    {
        _chosen.reset();
        return;
    }
    _chosen = (*_adapters)[choice->index];
    if (choice->outcome == RequestOutcome::FellBack)
        errorLog()("renderer.gpu: no GPU {} is present; using {}.", _selector, _chosen->candidate.title);
    startupLog()("GPU: '{}' {} (requested: {})", _chosen->candidate.title, _chosen->candidate.id, _selector);
}

void GraphicsDeviceSelector::applyTo(QQuickWindow& window)
{
    auto const& chosen = choose();
    if (auto* const instance = _lister->vulkanInstance())
        window.setVulkanInstance(instance);
    if (chosen && chosen->adapter)
        window.setGraphicsDevice(QQuickGraphicsDevice::fromRhiAdapter(chosen->adapter));
}

std::optional<AdapterEntry> const& GraphicsDeviceSelector::fallBackToAuto()
{
    _selector = config::GpuSelector {};
    if (_adapters)
        recompute();
    return choose();
}

std::optional<QRhi::Implementation> adapterImplementationFor(config::RenderingBackend backend)
{
    switch (backend)
    {
        case config::RenderingBackend::Vulkan: return QRhi::Vulkan;
        case config::RenderingBackend::Direct3D11: return QRhi::D3D11;
        case config::RenderingBackend::Direct3D12: return QRhi::D3D12;
#if defined(_WIN32)
        case config::RenderingBackend::Auto: return QRhi::D3D11; // Qt's default on Windows
#endif
        default: return std::nullopt;
    }
}

std::unique_ptr<IAdapterLister> makeQtAdapterLister(QRhi::Implementation implementation)
{
    return std::make_unique<QtAdapterLister>(implementation);
}

} // namespace contour::display
