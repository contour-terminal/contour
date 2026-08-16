// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/WaylandWindowShadow.hpp>

#if defined(CONTOUR_WAYLAND) && QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)

    #include <QtGui/QGuiApplication>
    #include <QtGui/QPainter>
    #include <QtGui/QWindow>
    #include <QtWaylandClient/QWaylandClientExtension>
    #include <QtWaylandClient/private/qwaylandshmbackingstore_p.h>
    #include <QtWaylandClient/private/qwaylandwindow_p.h>

    #include <array>
    #include <memory>
    #include <ranges>

    #include "qwayland-shadow.h"
    #include <qpa/qplatformwindow.h>

namespace contour::platform
{

namespace
{
    /// The compositor's shadow factory. A process-global registry binding, which is what a Wayland
    /// global IS -- unlike the per-window shadow state below, which is owned by its window.
    class KWinShadowManager:
        public QWaylandClientExtensionTemplate<KWinShadowManager>,
        public QtWayland::org_kde_kwin_shadow_manager
    {
      public:
        // Version 2, not 1 as the blur manager binds: version 2 is what adds the destroy request,
        // and the protocol has shipped at 2 since 2015.
        KWinShadowManager(): QWaylandClientExtensionTemplate<KWinShadowManager>(2) { initialize(); }
    };
    Q_GLOBAL_STATIC(KWinShadowManager, kwinShadowManager)

    [[nodiscard]] QtWaylandClient::QWaylandWindow* waylandWindowOf(QWindow& window) noexcept
    {
        return dynamic_cast<QtWaylandClient::QWaylandWindow*>(window.handle());
    }

    class WaylandWindowShadow final: public WindowShadow
    {
      public:
        explicit WaylandWindowShadow(QWindow& window) noexcept: _window { window } {}

        WaylandWindowShadow(WaylandWindowShadow const&) = delete;
        WaylandWindowShadow& operator=(WaylandWindowShadow const&) = delete;
        WaylandWindowShadow(WaylandWindowShadow&&) = delete;
        WaylandWindowShadow& operator=(WaylandWindowShadow&&) = delete;

        ~WaylandWindowShadow() override { withdraw(); }

        void apply(WindowShadowTiles const& tiles) override
        {
            if (tiles.isEmpty())
            {
                withdraw();
                return;
            }

            auto* manager = kwinShadowManager();
            if (manager == nullptr || !manager->isActive())
                return;

            auto* waylandWindow = waylandWindowOf(_window);
            if (waylandWindow == nullptr)
                return;

            auto* surface = waylandWindow->surface();
            if (surface == nullptr)
                return;

            if (tiles.geometry == _published && _shadow)
                return;

            // Buffers first: the old ones stay alive until the new set has been attached and
            // committed, so the compositor is never left holding a released buffer.
            auto buffers =
                std::array<std::unique_ptr<QtWaylandClient::QWaylandShmBuffer>, ShadowTileCount> {};
            for (auto const i: std::views::iota(size_t { 0 }, ShadowTileCount))
            {
                auto const& image = tiles.tiles[i];
                // Premultiplied, which is what wl_shm's ARGB8888 means -- the opposite of what the
                // X11 path needs, hence the conversion there and none here.
                auto buffer = std::make_unique<QtWaylandClient::QWaylandShmBuffer>(
                    waylandWindow->display(), image.size(), QImage::Format_ARGB32_Premultiplied);
                auto* target = buffer->image();
                if (target == nullptr || target->isNull())
                    return;

                // PAINTED into, not assigned. QWaylandShmBuffer::image() hands back a QImage that
                // merely wraps the shared-memory mapping the compositor reads, and QImage assignment
                // is a shallow, implicitly-shared rebind -- `*target = image` would repoint that
                // member at the tile's own pixels and abandon the mapping, leaving the compositor
                // reading the buffer exactly as allocated: fully transparent. The protocol traffic
                // looks perfect and no shadow appears.
                auto painter = QPainter { target };
                painter.setCompositionMode(QPainter::CompositionMode_Source);
                painter.drawImage(0, 0, image);
                painter.end();

                buffers[i] = std::move(buffer);
            }

            if (!_shadow)
                _shadow = std::make_unique<QtWayland::org_kde_kwin_shadow>(manager->create(surface));

            attachAll(buffers);

            auto const& offsets = tiles.geometry.offsets;
            _shadow->set_left_offset(wl_fixed_from_int(offsets.left));
            _shadow->set_top_offset(wl_fixed_from_int(offsets.top));
            _shadow->set_right_offset(wl_fixed_from_int(offsets.right));
            _shadow->set_bottom_offset(wl_fixed_from_int(offsets.bottom));
            _shadow->commit();

            _buffers = std::move(buffers);
            _published = tiles.geometry;

            // The shadow is part of the surface's double-buffered state: the protocol's own
            // documentation says to commit the wl_surface to apply the change. Without this a
            // shadow set while nothing is being drawn simply never appears.
            _window.requestUpdate();
        }

        void withdraw() override
        {
            if (!_shadow)
                return;

            if (auto* manager = kwinShadowManager(); manager != nullptr && manager->isActive())
                if (auto* waylandWindow = waylandWindowOf(_window))
                    if (auto* surface = waylandWindow->surface())
                    {
                        manager->unset(surface);
                        _window.requestUpdate();
                    }

            // Order matters: drop the shadow object before the buffers it referenced, or the
            // compositor is briefly pointed at buffers that no longer exist.
            _shadow->destroy();
            _shadow.reset();
            _buffers = {};
            _published = {};
        }

      private:
        using BufferSet = std::array<std::unique_ptr<QtWaylandClient::QWaylandShmBuffer>, ShadowTileCount>;

        void attachAll(BufferSet const& buffers)
        {
            // Eight separately named requests rather than an indexed one, so the mapping from tile
            // to request lives here once instead of at every call site.
            auto const attach = [&](ShadowTile tile,
                                    void (QtWayland::org_kde_kwin_shadow::*request)(wl_buffer*)) {
                (_shadow.get()->*request)(buffers[static_cast<size_t>(tile)]->buffer());
            };

            attach(ShadowTile::Top, &QtWayland::org_kde_kwin_shadow::attach_top);
            attach(ShadowTile::TopRight, &QtWayland::org_kde_kwin_shadow::attach_top_right);
            attach(ShadowTile::Right, &QtWayland::org_kde_kwin_shadow::attach_right);
            attach(ShadowTile::BottomRight, &QtWayland::org_kde_kwin_shadow::attach_bottom_right);
            attach(ShadowTile::Bottom, &QtWayland::org_kde_kwin_shadow::attach_bottom);
            attach(ShadowTile::BottomLeft, &QtWayland::org_kde_kwin_shadow::attach_bottom_left);
            attach(ShadowTile::Left, &QtWayland::org_kde_kwin_shadow::attach_left);
            attach(ShadowTile::TopLeft, &QtWayland::org_kde_kwin_shadow::attach_top_left);
        }

        QWindow& _window;
        std::unique_ptr<QtWayland::org_kde_kwin_shadow> _shadow;
        BufferSet _buffers {};
        ShadowGeometry _published {};
    };
} // namespace

std::unique_ptr<WindowShadow> makeWaylandWindowShadow(QWindow& window)
{
    if (QGuiApplication::platformName() != "wayland")
        return nullptr;

    auto* manager = kwinShadowManager();
    if (manager == nullptr || !manager->isActive())
        return nullptr;

    return std::make_unique<WaylandWindowShadow>(window);
}

} // namespace contour::platform

#endif
