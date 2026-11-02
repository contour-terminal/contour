// SPDX-License-Identifier: Apache-2.0
#include <contour/input/XkbLiveKeymap.hpp>

#include <QtGui/QGuiApplication>

#include <algorithm>
#include <utility>

#include <xkbcommon/xkbcommon-x11.h>
#include <xkbcommon/xkbcommon.h>

namespace contour::input
{

namespace
{
    struct ContextDeleter
    {
        void operator()(xkb_context* context) const noexcept { xkb_context_unref(context); }
    };

    struct KeymapDeleter
    {
        void operator()(xkb_keymap* keymap) const noexcept { xkb_keymap_unref(keymap); }
    };

    using ContextPtr = std::unique_ptr<xkb_context, ContextDeleter>;
    using KeymapPtr = std::unique_ptr<xkb_keymap, KeymapDeleter>;

    /// An XkbKeymap over a compiled libxkbcommon keymap.
    ///
    /// It holds no state of its own: the group arrives with each lookup, from the key event, so a lookup
    /// allocates nothing and asks nobody.
    class LibXkbKeymap final: public XkbKeymap
    {
      public:
        LibXkbKeymap(ContextPtr context, KeymapPtr keymap):
            _context { std::move(context) }, _keymap { std::move(keymap) }
        {
        }

        [[nodiscard]] uint32_t levelZeroKeysymOf(uint32_t keycode, uint32_t group) const noexcept override
        {
            auto const layouts = xkb_keymap_num_layouts_for_key(_keymap.get(), keycode);
            if (layouts == 0)
                return 0;

            xkb_keysym_t const* keysyms = nullptr;
            auto const count = xkb_keymap_key_get_syms_by_level(
                _keymap.get(), keycode, std::min<xkb_layout_index_t>(group, layouts - 1), 0, &keysyms);
            return count > 0 ? keysyms[0] : 0;
        }

      private:
        ContextPtr _context;
        KeymapPtr _keymap;
    };

    [[nodiscard]] ContextPtr newContext()
    {
        return ContextPtr { xkb_context_new(XKB_CONTEXT_NO_FLAGS) };
    }

#if QT_CONFIG(xcb)
    /// @return The X server connection of Qt's xcb platform; nullptr under any other.
    [[nodiscard]] xcb_connection_t* x11Connection()
    {
        if (qGuiApp == nullptr)
            return nullptr;
        auto const* native = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
        return native != nullptr ? native->connection() : nullptr;
    }
#endif
} // namespace

std::unique_ptr<XkbKeymap> makeXkbKeymapFromNames(XkbNames const& names)
{
    auto context = newContext();
    if (!context)
        return nullptr;

    auto const rmlvo = xkb_rule_names {
        .rules = names.rules.c_str(),
        .model = names.model.c_str(),
        .layout = names.layout.c_str(),
        .variant = names.variant.c_str(),
        .options = names.options.c_str(),
    };
    auto keymap = KeymapPtr { xkb_keymap_new_from_names(context.get(), &rmlvo, XKB_KEYMAP_COMPILE_NO_FLAGS) };
    if (!keymap)
        return nullptr;

    return std::make_unique<LibXkbKeymap>(std::move(context), std::move(keymap));
}

std::unique_ptr<XkbKeymap> makeLiveXkbKeymap()
{
#if QT_CONFIG(xcb)
    auto* const connection = x11Connection();
    if (connection == nullptr)
        return nullptr;

    if (!xkb_x11_setup_xkb_extension(connection,
                                     XKB_X11_MIN_MAJOR_XKB_VERSION,
                                     XKB_X11_MIN_MINOR_XKB_VERSION,
                                     XKB_X11_SETUP_XKB_EXTENSION_NO_FLAGS,
                                     nullptr,
                                     nullptr,
                                     nullptr,
                                     nullptr))
        return nullptr;

    auto const device = xkb_x11_get_core_keyboard_device_id(connection);
    if (device < 0)
        return nullptr;

    auto context = newContext();
    if (!context)
        return nullptr;

    auto keymap = KeymapPtr { xkb_x11_keymap_new_from_device(
        context.get(), connection, device, XKB_KEYMAP_COMPILE_NO_FLAGS) };
    if (!keymap)
        return nullptr;

    return std::make_unique<LibXkbKeymap>(std::move(context), std::move(keymap));
#else
    return nullptr;
#endif
}

} // namespace contour::input
