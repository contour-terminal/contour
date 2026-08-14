// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/XcbProperty.hpp>

#ifdef CONTOUR_FRONTEND_XCB

    #include <crispy/LogStore.hpp>

    #include <QtGui/QGuiApplication>

    #include <memory>

namespace contour::platform
{

namespace
{
    /// libxcb hands every reply to the caller as a malloc'd block the caller must free.
    struct FreeDeleter
    {
        void operator()(void* p) const noexcept { free(p); } // NOLINT(cppcoreguidelines-no-malloc)
    };

    template <typename T>
    using XcbReply = std::unique_ptr<T, FreeDeleter>;

    xcb_connection_t* x11Connection()
    {
        if (!qApp)
            return nullptr;

        auto* native = qApp->nativeInterface<QNativeInterface::QX11Application>();
        if (!native)
            return nullptr;

        return native->connection();
    }

    /// Writes @p length elements of @p data as property @p name of the given @p format and @p type.
    ///
    /// The three setPropertyX11 overloads differ only in these four values, so they share one body
    /// rather than three copies of the query-then-change-then-flush dance.
    void changeProperty(QWindow* window,
                        std::string const& name,
                        xcb_atom_t type,
                        uint8_t format,
                        uint32_t length,
                        void const* data)
    {
        auto const infoOpt = queryXcbPropertyInfo(window, name);
        if (!infoOpt)
        {
            errorLog()(R"(Could not resolve the X11 property "{}".)", name);
            return;
        }

        xcb_change_property(infoOpt->connection,
                            XCB_PROP_MODE_REPLACE,
                            infoOpt->window,
                            infoOpt->propertyAtom,
                            type,
                            format,
                            length,
                            data);
        xcb_flush(infoOpt->connection);
    }
} // namespace

std::optional<XcbPropertyInfo> queryXcbPropertyInfo(QWindow* window, std::string const& name)
{
    xcb_connection_t* xcbConnection = x11Connection();
    if (!xcbConnection)
        return std::nullopt;

    auto const winId = static_cast<xcb_window_t>(window->winId());

    auto const atomNameCookie =
        xcb_intern_atom(xcbConnection, 0, static_cast<uint16_t>(name.size()), name.c_str());
    // Owning, because xcb_intern_atom_reply() returns a block the caller must free -- the code this
    // was extracted from read reply->atom and dropped the pointer, leaking one reply per call.
    auto const reply =
        XcbReply<xcb_intern_atom_reply_t> { xcb_intern_atom_reply(xcbConnection, atomNameCookie, nullptr) };
    if (!reply)
        return std::nullopt;

    return XcbPropertyInfo { .connection = xcbConnection, .window = winId, .propertyAtom = reply->atom };
}

void setPropertyX11(QWindow* window, std::string const& name, uint32_t value)
{
    changeProperty(window, name, XCB_ATOM_CARDINAL, 32, 1, &value);
}

void setPropertyX11(QWindow* window, std::string const& name, std::string const& value)
{
    changeProperty(window, name, XCB_ATOM_STRING, 8, static_cast<uint32_t>(value.size()), value.data());
}

void setPropertyX11(QWindow* window, std::string const& name, std::span<uint32_t const> values)
{
    changeProperty(window, name, XCB_ATOM_CARDINAL, 32, static_cast<uint32_t>(values.size()), values.data());
}

void unsetPropertyX11(QWindow* window, std::string const& name)
{
    if (auto const infoOpt = queryXcbPropertyInfo(window, name))
    {
        xcb_delete_property(infoOpt->connection, infoOpt->window, infoOpt->propertyAtom);
        // The delete is as much a compositor-visible change as the set is, and the set flushes.
        // Without this the property lingers until something else happens to flush the connection.
        xcb_flush(infoOpt->connection);
    }
}

} // namespace contour::platform

#endif
