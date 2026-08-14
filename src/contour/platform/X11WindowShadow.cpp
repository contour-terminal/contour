// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/X11WindowShadow.hpp>

#ifdef CONTOUR_FRONTEND_XCB

    #include <crispy/LogStore.hpp>

    #include <QtGui/QGuiApplication>
    #include <QtGui/QWindow>

    #include <algorithm>
    #include <array>
    #include <ranges>
    #include <span>


namespace contour::platform
{

namespace
{
    /// The property KWin reads a client-decorated window's shadow from: eight pixmap ids followed
    /// by the four offsets, as twelve CARDINAL values.
    /// @see kwin/src/shadow.cpp, readX11ShadowProperty.
    constexpr auto ShadowPropertyName = "_KDE_NET_WM_SHADOW";

    /// Bytes of request header xcb_put_image needs beyond the image data itself. Generous on
    /// purpose: the cost of overestimating is one extra band, and of underestimating is a protocol
    /// error that kills the connection.
    constexpr auto PutImageHeaderBytes = uint32_t { 64 };

    /// Whether the server lays out image bytes the way QImage::Format_ARGB32 does.
    ///
    /// ARGB32 is host-endian 0xAARRGGBB, i.e. B,G,R,A in memory on a little-endian machine. If the
    /// server wants the other order, every pixel we upload would have its channels swapped, so the
    /// honest answer is to publish no shadow rather than a blue one.
    [[nodiscard]] bool hasUsableByteOrder(xcb_connection_t* connection) noexcept
    {
        auto const* setup = xcb_get_setup(connection);
        return setup != nullptr && setup->image_byte_order == XCB_IMAGE_ORDER_LSB_FIRST;
    }

    /// Whether the server can hold the 32-bit-per-pixel pixmaps the tiles are uploaded as.
    [[nodiscard]] bool hasDepth32Pixmaps(xcb_connection_t* connection) noexcept
    {
        auto const* setup = xcb_get_setup(connection);
        if (setup == nullptr)
            return false;

        auto const formats = std::span { xcb_setup_pixmap_formats(setup),
                                         static_cast<size_t>(xcb_setup_pixmap_formats_length(setup)) };
        return std::ranges::any_of(formats, [](xcb_format_t const& format) {
            return format.depth == 32 && format.bits_per_pixel == 32;
        });
    }

    class X11WindowShadow final: public WindowShadow
    {
      public:
        X11WindowShadow(xcb_connection_t* connection,
                        xcb_window_t windowId,
                        xcb_atom_t propertyAtom,
                        uint32_t maxRequestBytes) noexcept:
            _connection { connection },
            _windowId { windowId },
            _propertyAtom { propertyAtom },
            _maxRequestBytes { maxRequestBytes }
        {
        }

        X11WindowShadow(X11WindowShadow const&) = delete;
        X11WindowShadow& operator=(X11WindowShadow const&) = delete;
        X11WindowShadow(X11WindowShadow&&) = delete;
        X11WindowShadow& operator=(X11WindowShadow&&) = delete;

        ~X11WindowShadow() override { withdraw(); }

        void apply(WindowShadowTiles const& tiles) override
        {
            if (tiles.isEmpty())
            {
                withdraw();
                return;
            }

            // Re-uploading an unchanged shadow costs eight pixmaps and a round trip for nothing,
            // and the caller re-asserts on every window state change.
            if (tiles.geometry == _published && _pixmaps.front() != XCB_PIXMAP_NONE)
                return;

            auto const previous = _pixmaps;
            auto created = std::array<xcb_pixmap_t, ShadowTileCount> {};
            auto gc = xcb_gcontext_t { XCB_NONE };

            for (auto const i: std::views::iota(size_t { 0 }, ShadowTileCount))
            {
                // Non-premultiplied: KWin reads the pixmap back as QImage::Format_ARGB32, so
                // premultiplied bytes would render a visibly too-dark shadow.
                auto const image = tiles.tiles[i].convertToFormat(QImage::Format_ARGB32);
                auto const width = image.width();
                auto const height = image.height();
                if (width <= 0 || height <= 0 || image.bytesPerLine() != width * 4)
                {
                    releasePixmaps(created);
                    if (gc != XCB_NONE)
                        xcb_free_gc(_connection, gc);
                    errorLog()("Window shadow tile {} has an unexpected layout; not publishing.", i);
                    return;
                }

                created[i] = xcb_generate_id(_connection);
                // The window is the drawable only to name the SCREEN; a depth-32 pixmap on a
                // depth-24 window is fine, which is why the graphics context below cannot be
                // created against the window and is created against the pixmap instead.
                xcb_create_pixmap(_connection,
                                  32,
                                  created[i],
                                  _windowId,
                                  static_cast<uint16_t>(width),
                                  static_cast<uint16_t>(height));

                if (gc == XCB_NONE)
                {
                    gc = xcb_generate_id(_connection);
                    xcb_create_gc(_connection, gc, created[i], 0, nullptr);
                }

                uploadImage(created[i], gc, image);
            }

            if (gc != XCB_NONE)
                xcb_free_gc(_connection, gc);

            _pixmaps = created;
            publishProperty(tiles.geometry);
            _published = tiles.geometry;

            // KWin COPIES the pixmaps (xcb_get_image on the property change) rather than holding a
            // reference, so the previous generation may go -- but only once the server has actually
            // processed the property change, which is what this round trip waits for.
            roundTrip();
            releasePixmaps(previous);
        }

        void withdraw() override
        {
            if (_pixmaps.front() == XCB_PIXMAP_NONE && _published == ShadowGeometry {})
                return;

            xcb_delete_property(_connection, _windowId, _propertyAtom);
            xcb_flush(_connection);
            roundTrip();
            releasePixmaps(_pixmaps);
            _pixmaps = {};
            _published = {};
        }

      private:
        void uploadImage(xcb_pixmap_t pixmap, xcb_gcontext_t gc, QImage const& image)
        {
            auto const stride = static_cast<uint32_t>(image.bytesPerLine());
            auto const height = static_cast<uint32_t>(image.height());
            auto const width = static_cast<uint16_t>(image.width());

            // A whole tile can exceed the server's maximum request length -- a VeryLarge corner
            // tile is over 256 KiB, which is the classic limit when BIG-REQUESTS is unavailable --
            // so the upload goes in bands of whole rows.
            auto const budget =
                _maxRequestBytes > PutImageHeaderBytes ? _maxRequestBytes - PutImageHeaderBytes : stride;
            auto const rowsPerBand = std::max(uint32_t { 1 }, budget / std::max(uint32_t { 1 }, stride));

            auto const bandCount = (height + rowsPerBand - 1) / rowsPerBand;
            for (auto const band: std::views::iota(uint32_t { 0 }, bandCount))
            {
                auto const row = band * rowsPerBand;
                auto const rows = std::min(rowsPerBand, height - row);
                xcb_put_image(_connection,
                              XCB_IMAGE_FORMAT_Z_PIXMAP,
                              pixmap,
                              gc,
                              width,
                              static_cast<uint16_t>(rows),
                              0,
                              static_cast<int16_t>(row),
                              0,
                              32,
                              rows * stride,
                              image.constScanLine(static_cast<int>(row)));
            }
        }

        void publishProperty(ShadowGeometry const& geometry)
        {
            // Eight pixmap ids in ShadowTile order, then top, right, bottom, left -- the order
            // kwin/src/shadow.cpp reads them back in.
            auto values = std::array<uint32_t, ShadowTileCount + 4> {};
            std::ranges::copy(_pixmaps, values.begin());
            values[ShadowTileCount + 0] = static_cast<uint32_t>(geometry.offsets.top);
            values[ShadowTileCount + 1] = static_cast<uint32_t>(geometry.offsets.right);
            values[ShadowTileCount + 2] = static_cast<uint32_t>(geometry.offsets.bottom);
            values[ShadowTileCount + 3] = static_cast<uint32_t>(geometry.offsets.left);

            // Straight to xcb rather than through setPropertyX11: the atom was interned once, in
            // the factory, and that helper would re-intern it on every publish -- a synchronous
            // round trip, which over a forwarded X connection is a full network round trip.
            xcb_change_property(_connection,
                                XCB_PROP_MODE_REPLACE,
                                _windowId,
                                _propertyAtom,
                                XCB_ATOM_CARDINAL,
                                32,
                                static_cast<uint32_t>(values.size()),
                                values.data());
            xcb_flush(_connection);
        }

        /// Waits until the server has processed everything queued so far.
        void roundTrip()
        {
            auto const cookie = xcb_get_input_focus(_connection);
            auto const reply = XcbReply<xcb_get_input_focus_reply_t> {
                xcb_get_input_focus_reply(_connection, cookie, nullptr)
            };
        }

        void releasePixmaps(std::array<xcb_pixmap_t, ShadowTileCount> const& pixmaps) noexcept
        {
            for (auto const pixmap: pixmaps)
                if (pixmap != XCB_PIXMAP_NONE)
                    xcb_free_pixmap(_connection, pixmap);
        }

        xcb_connection_t* _connection;
        xcb_window_t _windowId;
        xcb_atom_t _propertyAtom;
        uint32_t _maxRequestBytes;
        std::array<xcb_pixmap_t, ShadowTileCount> _pixmaps {};
        ShadowGeometry _published {};
    };
} // namespace

std::unique_ptr<WindowShadow> makeX11WindowShadow(QWindow& window)
{
    if (QGuiApplication::platformName() != "xcb")
        return nullptr;

    auto const info = queryXcbPropertyInfo(&window, ShadowPropertyName);
    if (!info)
        return nullptr;

    if (!hasUsableByteOrder(info->connection) || !hasDepth32Pixmaps(info->connection))
    {
        errorLog()("This X server cannot carry a window shadow; continuing without one.");
        return nullptr;
    }

    // In 4-byte units, hence the multiply.
    auto const maxRequestBytes = xcb_get_maximum_request_length(info->connection) * 4;

    return std::make_unique<X11WindowShadow>(
        info->connection, info->window, info->propertyAtom, maxRequestBytes);
}

} // namespace contour::platform

#endif
