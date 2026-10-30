// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/TaskbarOverlay.hpp>
#include <contour/platform/TaskbarProgressSink.hpp>
#include <contour/platform/TaskbarProgressState.hpp>

#ifdef _WIN32
    #include <core/log/LogStore.hpp>

    #include <QtCore/QAbstractNativeEventFilter>
    #include <QtCore/QCoreApplication>
    #include <QtCore/QPointer>
    #include <QtCore/QRect>
    #include <QtGui/QColor>
    #include <QtGui/QFont>
    #include <QtGui/QGuiApplication>
    #include <QtGui/QImage>
    #include <QtGui/QPainter>
    #include <QtGui/QPen>
    #include <QtGui/QPlatformSurfaceEvent>
    #include <QtGui/QWindow>

    #include <algorithm>
    #include <atomic>
    #include <cmath>
    #include <expected>
    #include <memory>
    #include <type_traits>
    #include <unordered_map>
    #include <vector>

    #include <ShObjIdl.h>
    #include <Windows.h>

    #include <wrl/client.h>
#endif

#if !defined(__linux__) && !defined(__APPLE__)
namespace contour::platform
{
namespace
{
    class NullTaskbarProgressSink final: public TaskbarProgressSink
    {
      public:
        [[nodiscard]] bool isSupported() const noexcept override { return false; }
        [[nodiscard]] TaskbarProgressScope scope() const noexcept override
        {
            return TaskbarProgressScope::Window;
        }
        void setApplicationProgress(TaskbarProgress) override {}
        void setApplicationAttention(TaskbarAttention) override {}
        void bindWindow(uint64_t, QWindow&) override {}
        void setProgress(uint64_t, TaskbarProgress) override {}
        void setAttention(uint64_t, TaskbarAttention) override {}
        void removeWindow(uint64_t) override {}
    };

    #ifdef _WIN32
    auto const taskbarLog = core::log::Category { "taskbar.progress", "Windows taskbar progress" };

    std::atomic<size_t> liveOverlayIcons = 0; ///< HICONs created by makeOverlayIcon() and not yet destroyed.

    /// Destroys an overlay icon and keeps liveOverlayIcons() true.
    struct OverlayIconDeleter
    {
        void operator()(HICON icon) const noexcept
        {
            DestroyIcon(icon);
            liveOverlayIcons.fetch_sub(1, std::memory_order_relaxed);
        }
    };
    using OverlayIcon = std::unique_ptr<std::remove_pointer_t<HICON>, OverlayIconDeleter>;

    /// Paints the overlay: a disc in OverlayDiscColor with the count in the ink chosen for it.
    /// @param attention The count to draw; it is shown nonzero.
    /// @param pixelRatio The window's device-pixel ratio, so the icon stays crisp on a scaled display.
    /// @return The icon, or null when Windows refused it.
    [[nodiscard]] OverlayIcon makeOverlayIcon(TaskbarAttention attention, qreal pixelRatio)
    {
        constexpr auto LogicalSize = 16;
        auto const size = static_cast<int>(std::ceil(LogicalSize * pixelRatio));
        auto image = QImage(size, size, QImage::Format_ARGB32);
        image.fill(Qt::transparent);
        {
            auto const disc = QColor(OverlayDiscColor.red, OverlayDiscColor.green, OverlayDiscColor.blue);
            auto const ink = overlayInkFor(OverlayDiscColor);
            auto const outline =
                QColor(OverlayOutlineColor.red, OverlayOutlineColor.green, OverlayOutlineColor.blue);
            auto const ringWidth = std::max(1.0, OverlayOutlineWidth * pixelRatio);
            auto const text = QString::fromStdString(overlayBadgeText(attention.waiting));
            auto painter = QPainter(&image);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(QPen(outline, ringWidth));
            painter.setBrush(disc);
            // Inset by half the ring so that it stays inside the icon's edge.
            auto const ringInset = ringWidth / 2.0;
            painter.drawEllipse(QRectF(image.rect()).adjusted(ringInset, ringInset, -ringInset, -ringInset));
            auto font = painter.font();
            font.setPixelSize(overlayTextPixelSize(size, static_cast<size_t>(text.size())));
            font.setBold(true);
            painter.setFont(font);
            painter.setPen(QColor(ink.red, ink.green, ink.blue));
            painter.drawText(image.rect(), Qt::AlignCenter, text);
        }
        auto const color = CreateBitmap(size, size, 1, 32, image.constBits());
        // The 1 bpp mask has rows padded to 16 bits; all zero, as the colour bitmap's alpha does the masking.
        auto const maskRowBytes = (size + 15) / 16 * 2;
        auto const maskBits = std::vector<uchar>(static_cast<size_t>(maskRowBytes) * size, 0);
        auto const mask = CreateBitmap(size, size, 1, 1, maskBits.data());
        auto info =
            ICONINFO { .fIcon = TRUE, .xHotspot = 0, .yHotspot = 0, .hbmMask = mask, .hbmColor = color };
        auto const icon = color && mask ? CreateIconIndirect(&info) : nullptr;
        if (color)
            DeleteObject(color);
        if (mask)
            DeleteObject(mask);
        if (!icon)
            return nullptr;
        liveOverlayIcons.fetch_add(1, std::memory_order_relaxed);
        return OverlayIcon { icon };
    }

    /// Application-owned native observer; HWNDs are read only while Qt owns a live native surface.
    class Win32TaskbarProgressSink final:
        public QObject,
        public TaskbarProgressSink,
        public QAbstractNativeEventFilter
    {
      public:
        [[nodiscard]] bool isSupported() const noexcept override { return true; }
        [[nodiscard]] TaskbarProgressScope scope() const noexcept override
        {
            return TaskbarProgressScope::Window;
        }
        void setApplicationProgress(TaskbarProgress) override {}
        void setApplicationAttention(TaskbarAttention) override {}
        ~Win32TaskbarProgressSink() override
        {
            if (auto* app = QCoreApplication::instance())
                app->removeNativeEventFilter(this);
            _taskbar.Reset();
            if (_apartment == Apartment::Owned)
                CoUninitialize();
        }

        void bindWindow(uint64_t id, QWindow& window) override
        {
            if (QGuiApplication::platformName() != QStringLiteral("windows"))
                return;
            if (auto const found = _windows.find(id); found != _windows.end())
            {
                if (found->second.window == &window)
                    return;
                removeWindow(id);
            }
            if (!_buttonCreatedMessage)
            {
                _buttonCreatedMessage = RegisterWindowMessageW(L"TaskbarButtonCreated");
                if (!_buttonCreatedMessage)
                    return;
                QCoreApplication::instance()->installNativeEventFilter(this);
            }
            _windows.emplace(id, Window { .window = &window });
            window.installEventFilter(this);
            allowButtonMessage(_windows.at(id));
            connect(&window, &QObject::destroyed, this, [this, id] { _windows.erase(id); });
            // A new screen may scale differently: the overlay icon is drawn for one pixel ratio, so it is
            // sent again, rebuilt at the new one.
            connect(&window, &QWindow::screenChanged, this, [this, id] {
                if (auto const found = _windows.find(id); found != _windows.end())
                {
                    found->second.overlay.invalidate();
                    publish(found->second);
                }
            });
        }

        void setProgress(uint64_t id, TaskbarProgress progress) override
        {
            if (auto const found = _windows.find(id); found != _windows.end())
            {
                found->second.progress.set(progress);
                publish(found->second);
            }
        }

        void setAttention(uint64_t id, TaskbarAttention attention) override
        {
            if (auto const found = _windows.find(id); found != _windows.end())
            {
                found->second.overlay.set(attention);
                publish(found->second);
            }
        }

        void removeWindow(uint64_t id) override
        {
            if (auto const found = _windows.find(id); found != _windows.end())
            {
                auto& entry = found->second;
                entry.progress.set({});
                entry.overlay.set({});
                publish(entry);
                if (entry.window)
                {
                    entry.window->removeEventFilter(this);
                    disconnect(entry.window, nullptr, this, nullptr);
                }
                _windows.erase(found);
            }
        }

      protected:
        bool eventFilter(QObject* watched, QEvent* event) override
        {
            if (event->type() == QEvent::PlatformSurface)
                for (auto& [id, entry]: _windows)
                    if (entry.window == watched)
                    {
                        if (static_cast<QPlatformSurfaceEvent*>(event)->surfaceEventType()
                            == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed)
                        {
                            entry.progress.surfaceDestroyed();
                            entry.overlay.surfaceDestroyed();
                        }
                        else
                            allowButtonMessage(entry);
                    }
            return QObject::eventFilter(watched, event);
        }

        bool nativeEventFilter(QByteArray const& eventType, void* message, qintptr*) override
        {
            if (eventType != "windows_generic_MSG")
                return false;
            auto const& native = *static_cast<MSG*>(message);
            if (native.message == _buttonCreatedMessage)
                for (auto& [id, entry]: _windows)
                    if (handle(entry) == native.hwnd)
                    {
                        _taskbar.Reset(); // Explorer may have recreated the button after a restart.
                        entry.progress.buttonCreated();
                        entry.overlay.buttonCreated();
                        publish(entry);
                        break;
                    }
            return false; // Qt and other native observers still receive the message.
        }

      private:
        enum class Apartment : uint8_t
        {
            Uninitialized,
            Borrowed,
            Owned,
        };

        struct Window
        {
            QPointer<QWindow> window;
            TaskbarProgressState progress {};
            TaskbarAttentionState overlay {};
            OverlayIcon icon {}; ///< The overlay icon the taskbar button currently shows, owned here.
            std::optional<HRESULT> lastFailure {};
        };

        [[nodiscard]] static HWND handle(Window const& entry)
        {
            return entry.window && entry.window->handle() ? reinterpret_cast<HWND>(entry.window->winId())
                                                          : nullptr;
        }

        void allowButtonMessage(Window const& entry) const
        {
            // Explorer normally has medium integrity, even when Contour is elevated. Allow only
            // its registered readiness message, on this HWND, rather than changing the process filter.
            if (auto const hwnd = handle(entry);
                hwnd && !ChangeWindowMessageFilterEx(hwnd, _buttonCreatedMessage, MSGFLT_ALLOW, nullptr))
                taskbarLog()("Cannot receive TaskbarButtonCreated: Windows error {}", GetLastError());
        }

        [[nodiscard]] HRESULT acquireTaskbar()
        {
            if (_apartment == Apartment::Uninitialized)
            {
                auto const result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                if (FAILED(result) && result != RPC_E_CHANGED_MODE)
                    return result;
                _apartment = SUCCEEDED(result) ? Apartment::Owned : Apartment::Borrowed;
            }
            if (!_taskbar)
            {
                auto result = CoCreateInstance(
                    CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&_taskbar));
                if (FAILED(result))
                    return result;
                result = _taskbar->HrInit();
                if (FAILED(result))
                    _taskbar.Reset();
                return result;
            }
            return S_OK;
        }

        void publish(Window& entry)
        {
            publishProgress(entry);
            publishOverlay(entry);
        }

        void publishOverlay(Window& entry)
        {
            auto const attention = entry.overlay.pending();
            auto const hwnd = handle(entry);
            if (!attention || !hwnd)
                return;
            auto result = acquireTaskbar();
            auto icon = OverlayIcon {};
            if (SUCCEEDED(result))
            {
                // A null icon clears the overlay. The shell copies the icon, so the previous one is
                // destroyed only once the new one is in place.
                if (attention->waiting != 0)
                    icon = makeOverlayIcon(*attention, entry.window->devicePixelRatio());
                auto const description =
                    QString::fromStdString(overlayDescription(attention->waiting)).toStdWString();
                result = _taskbar->SetOverlayIcon(hwnd, icon.get(), description.c_str());
            }
            if (SUCCEEDED(result))
            {
                entry.icon = std::move(icon);
                entry.overlay.applied();
                entry.lastFailure.reset();
                taskbarLog()("Window {}: overlay {}", reinterpret_cast<uintptr_t>(hwnd), attention->waiting);
            }
            else
            {
                if (entry.lastFailure != result)
                    taskbarLog()("Taskbar overlay update failed: HRESULT {:08X}",
                                 static_cast<uint32_t>(result));
                entry.lastFailure = result;
                _taskbar.Reset(); // Reacquire on the next report or TaskbarButtonCreated.
            }
        }

        void publishProgress(Window& entry)
        {
            auto const progress = entry.progress.pending();
            auto const hwnd = handle(entry);
            if (!progress || !hwnd)
                return;
            auto result = acquireTaskbar();
            if (SUCCEEDED(result))
            {
                auto const checked = [](HRESULT status) -> std::expected<void, HRESULT> {
                    if (FAILED(status))
                        return std::unexpected(status);
                    return {};
                };
                auto const applied = applyWindowsTaskbarProgress(
                    *progress,
                    [&](uint8_t percentage) {
                        return checked(_taskbar->SetProgressValue(hwnd, percentage, 100));
                    },
                    [&](vtbackend::ProgressState desired) {
                        auto state = TBPF_NOPROGRESS;
                        switch (desired)
                        {
                            case vtbackend::ProgressState::Inactive: break;
                            case vtbackend::ProgressState::Normal: state = TBPF_NORMAL; break;
                            case vtbackend::ProgressState::Error: state = TBPF_ERROR; break;
                            case vtbackend::ProgressState::Indeterminate: state = TBPF_INDETERMINATE; break;
                            case vtbackend::ProgressState::Paused: state = TBPF_PAUSED; break;
                        }
                        return checked(_taskbar->SetProgressState(hwnd, state));
                    });
                result = applied ? S_OK : applied.error();
            }
            if (SUCCEEDED(result))
            {
                entry.progress.applied();
                entry.lastFailure.reset();
                taskbarLog()("Window {}: state {}, progress {}%",
                             reinterpret_cast<uintptr_t>(hwnd),
                             std::to_underlying(progress->state),
                             progress->percentage);
            }
            else
            {
                if (entry.lastFailure != result)
                    taskbarLog()("Taskbar progress update failed: HRESULT {:08X}",
                                 static_cast<uint32_t>(result));
                entry.lastFailure = result;
                _taskbar.Reset(); // Reacquire on the next report or TaskbarButtonCreated.
            }
        }

        UINT _buttonCreatedMessage = 0;
        Apartment _apartment = Apartment::Uninitialized;
        Microsoft::WRL::ComPtr<ITaskbarList3> _taskbar;
        std::unordered_map<uint64_t, Window> _windows;
    };
    #endif
} // namespace

    #ifdef _WIN32
size_t liveOverlayIconCount() noexcept
{
    return liveOverlayIcons.load(std::memory_order_relaxed);
}
    #endif

std::unique_ptr<TaskbarProgressSink> makeTaskbarProgressSink()
{
    #ifdef _WIN32
    return std::make_unique<Win32TaskbarProgressSink>();
    #else
    return std::make_unique<NullTaskbarProgressSink>();
    #endif
}
} // namespace contour::platform
#endif
