// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/QtPath.hpp>
#include <contour/window/SettingsController.hpp>

#include <vtbackend/screen/StatusLineBuilder.hpp>

#include <text_shaper/Font.hpp>

#include <QtCore/QStringList>
#include <QtGui/QColor>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <format>
#include <set>
#include <string_view>
#include <utility>

namespace contour::window
{

using config::SettingsOrigin;
using config::TerminalProfile;
using namespace std::string_view_literals;

namespace
{
    /// A data-driven descriptor for one editable scalar profile field: adding a field to the settings
    /// page is adding a row here, not editing logic in three places.
    struct ProfileFieldDescriptor
    {
        QString key;   //!< Stable identifier / QML key.
        QString label; //!< Human-readable label.
        QString help;  //!< One-line help text.
        QString type;  //!< "bool" | "int" | "double" | "string" | "enum".
        std::function<QVariant(TerminalProfile const&)> get;        //!< Reads the field as a QVariant.
        std::function<void(TerminalProfile&, QVariant const&)> set; //!< Writes the field from QVariant.
        QStringList options {}; //!< For "enum": the allowed values (the combo model + accepted set).
    };

    /// One titled group of related profile fields, as the settings page shows them.
    ///
    /// The grouping is data, not presentation: the page renders one collapsible section per group, and a
    /// field's section is decided here, next to the field. These boundaries previously existed only as
    /// `// {{{ … }}}` comments around the descriptor list, which left the page rendering ninety
    /// undifferentiated rows in a single flat column -- and left it hiding the indicator fields by
    /// matching on a key prefix in QML, a rule no C++ reader of the descriptor list could see.
    struct ProfileFieldGroup
    {
        QString title;                              //!< Section heading.
        QString glyph;                              //!< Leading pictograph for the heading.
        std::vector<ProfileFieldDescriptor> fields; //!< The fields shown under it, in order.
    };

    /// Builds a bool profile-field descriptor from a ConfigEntry member accessor.
    template <typename Accessor>
    ProfileFieldDescriptor boolField(QString key, QString label, QString help, Accessor accessor)
    {
        return { std::move(key),
                 std::move(label),
                 std::move(help),
                 "bool",
                 [accessor](TerminalProfile const& p) { return QVariant(accessor(p).value()); },
                 [accessor](TerminalProfile& p, QVariant const& v) { accessor(p) = v.toBool(); } };
    }

    [[nodiscard]] QString toQString(std::string_view text)
    {
        return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
    }

    /// The configuration tokens of every value of configuration enum @p Enum, in table order.
    template <typename Enum>
    [[nodiscard]] QStringList configEnumTokens()
    {
        auto tokens = QStringList {};
        for (auto const& info: config::configEnumValues<Enum>())
            tokens.push_back(toQString(info.token));
        return tokens;
    }

    /// Every cursor shape paired with the token a configuration file names it by.
    ///
    /// Mirrors what vtbackend::makeCursorShape() accepts. It is spelled out here rather than derived
    /// from that function because the function only parses -- it maps a token to a shape and throws on
    /// anything else, so it can neither name a shape nor list the shapes a combo box should offer.
    constexpr auto CursorShapeNames = std::array {
        std::pair { "block"sv, vtbackend::CursorShape::Block },
        std::pair { "rectangle"sv, vtbackend::CursorShape::Rectangle },
        std::pair { "underscore"sv, vtbackend::CursorShape::Underscore },
        std::pair { "bar"sv, vtbackend::CursorShape::Bar },
    };

    /// One of the profile's `history:` depths, in the configuration's own `-1 == unlimited` spelling.
    ///
    /// A pointer-to-member rather than a getter/setter pair, because the two rows this builds differ
    /// in nothing else: written out by hand they were character-for-character identical but for the
    /// field name, and a third depth tomorrow would have been a third copy of the same encoding.
    ///
    /// -1 in both directions, matching what the HistoryConfig writer emits, so what the page shows is
    /// what the file holds. Mapping Infinite to 0 on the way out while reading 0 back as LineCount(0)
    /// -- as this once did -- displayed unlimited scrollback as "0" and then turned it into literally
    /// no scrollback the first time the field was touched.
    ProfileFieldDescriptor historyLimitField(QString key,
                                             QString label,
                                             QString help,
                                             vtbackend::MaxHistoryLineCount config::HistoryConfig::* field)
    {
        return { std::move(key),
                 std::move(label),
                 std::move(help),
                 "int",
                 [field](TerminalProfile const& p) {
                     auto const& limit = p.history.value().*field;
                     if (auto const* lineCount = std::get_if<vtbackend::LineCount>(&limit))
                         return QVariant(static_cast<int>(unbox(*lineCount)));
                     return QVariant(-1);
                 },
                 [field](TerminalProfile& p, QVariant const& v) {
                     auto history = p.history.value();
                     auto const lines = v.toInt();
                     if (lines < 0)
                         history.*field = vtbackend::Infinite {};
                     else
                         history.*field = vtbackend::LineCount(lines);
                     p.history = history;
                 } };
    }

    /// enumField() for an enum that already has a ConfigEnum table.
    ///
    /// The overload below takes the token/value pairs by hand, which for a table-backed enum means
    /// spelling every value a second time, in a file that no test compares against the table. This
    /// one reads the table, so adding a value stays what ConfigEnum.hpp promises it is: adding a row.
    template <typename Enum, typename Getter, typename Setter>
    ProfileFieldDescriptor enumFieldFromTable(
        QString key, QString label, QString help, Getter getter, Setter setter)
    {
        return { std::move(key),
                 std::move(label),
                 std::move(help),
                 "enum",
                 [getter](TerminalProfile const& p) -> QVariant {
                     return QVariant(toQString(config::configEnumToken(getter(p))));
                 },
                 [setter](TerminalProfile& p, QVariant const& v) {
                     if (auto const value = config::configEnumFromToken<Enum>(v.toString().toStdString()))
                         setter(p, *value);
                 },
                 configEnumTokens<Enum>() };
    }

    /// Builds an enum profile-field descriptor from a token-to-value table.
    ///
    /// The combo-box options, the getter's display token and the setter's parse are all derived from
    /// @p mappings, so the set of values a user can pick and the set the setter understands cannot drift
    /// apart. Both halves used to be written out by hand -- an option list beside a chain of string
    /// comparisons -- while the getter derived its token from `std::format`, a third spelling again:
    /// the formatters render values for people ("host writable", "Logical", "Bold"), not as the tokens
    /// this option list and the config grammar use. A `Decorator` field would have shown nothing
    /// selected, because "dotted-underline" lowercases to itself but "DottedUnderline" does not.
    template <typename Enum, size_t N, typename Getter, typename Setter>
    ProfileFieldDescriptor enumField(QString key,
                                     QString label,
                                     QString help,
                                     std::array<std::pair<std::string_view, Enum>, N> mappings,
                                     Getter getter,
                                     Setter setter)
    {
        auto options = QStringList {};
        for (auto const& [token, _]: mappings)
            options.push_back(toQString(token));

        return { std::move(key),
                 std::move(label),
                 std::move(help),
                 "enum",
                 [mappings, getter](TerminalProfile const& p) -> QVariant {
                     auto const current = getter(p);
                     for (auto const& [token, value]: mappings)
                         if (value == current)
                             return QVariant(toQString(token));
                     // A value this table has no token for cannot be shown in the combo box; an empty
                     // string leaves it unselected rather than silently displaying a neighbour's token.
                     return QVariant { QString {} };
                 },
                 [mappings, setter](TerminalProfile& p, QVariant const& v) {
                     auto const token = v.toString().toStdString();
                     for (auto const& [candidate, value]: mappings)
                         if (candidate == token)
                         {
                             setter(p, value);
                             return;
                         }
                 },
                 std::move(options) };
    }

    /// The editable scalar profile fields, in the groups the settings page shows them under.
    ///
    /// Data-driven twice over: it grows one row at a time toward full parity without touching any other
    /// code (the page renders each row by its `type`), and a row's section is the group it sits in (the
    /// page renders one collapsible card per group). Nothing here knows what a section looks like, and
    /// nothing in QML knows which fields belong together.
    std::vector<ProfileFieldGroup> const& profileFieldGroups()
    {
        using vtbackend::CursorShape;
        using vtbackend::LineCount;
        using vtbackend::LineOffset;
        using Ms = std::chrono::milliseconds;
        static auto const groups = std::vector<ProfileFieldGroup> {
            { "Window",
              "▭",
              {
                  boolField("show_title_bar",
                            "Show title bar",
                            "Whether the window shows its title bar.",
                            [](auto& p) -> auto& { return p.showTitleBar; }),
                  boolField("maximized",
                            "Start maximized",
                            "Open the window maximized.",
                            [](auto& p) -> auto& { return p.maximized; }),
                  boolField("fullscreen",
                            "Start fullscreen",
                            "Open the window in fullscreen.",
                            [](auto& p) -> auto& { return p.fullscreen; }),
                  boolField("size_indicator_on_resize",
                            "Show size on resize",
                            "Briefly show the terminal dimensions when the window is resized.",
                            [](auto& p) -> auto& { return p.sizeIndicatorOnResize; }),
                  { "dim_unfocused",
                    "Dim when unfocused",
                    "How much to dim a pane while it is not focused (0.0 = off, 1.0 = fully dimmed).",
                    "double",
                    [](TerminalProfile const& p) { return QVariant(p.dimUnfocused.value()); },
                    [](TerminalProfile& p, QVariant const& v) { p.dimUnfocused = v.toDouble(); } },
              } },
            { "Font",
              "A",
              {
                  { "font_family",
                    "Font family",
                    "The regular font family name.",
                    "string",
                    [](TerminalProfile const& p) {
                        return QVariant(QString::fromStdString(p.fonts.value().regular.familyName));
                    },
                    [](TerminalProfile& p, QVariant const& v) {
                        p.fonts.value().regular.familyName = v.toString().toStdString();
                    } },
                  { "font_size",
                    "Font size (pt)",
                    "The font size in points.",
                    "double",
                    [](TerminalProfile const& p) { return QVariant(p.fonts.value().size.pt); },
                    [](TerminalProfile& p, QVariant const& v) {
                        p.fonts.value().size = text::FontSize { v.toDouble() };
                    } },
                  boolField("draw_bold_text_with_bright_colors",
                            "Bold text uses bright colors",
                            "Render bold text using the bright ANSI colors.",
                            [](auto& p) -> auto& { return p.drawBoldTextWithBrightColors; }),
              } },
            { "Scrolling",
              "↕",
              {
                  boolField("smooth_scrolling",
                            "Smooth scrolling",
                            "Animate scrolling between lines.",
                            [](auto& p) -> auto& { return p.smoothScrolling; }),
                  boolField("momentum_scrolling",
                            "Momentum scrolling",
                            "Continue scrolling with momentum.",
                            [](auto& p) -> auto& { return p.momentumScrolling; }),
                  { "slow_scrolling_time",
                    "Slow scrolling time (ms)",
                    "Duration of one smooth line-scroll step, in milliseconds.",
                    "int",
                    [](TerminalProfile const& p) {
                        return QVariant(static_cast<int>(p.smoothLineScrolling.value().count()));
                    },
                    [](TerminalProfile& p, QVariant const& v) { p.smoothLineScrolling = Ms { v.toInt() }; } },
                  { "vi_mode_scrolloff",
                    "Vi mode scroll-off",
                    "Minimum lines kept above/below the cursor while scrolling in Vi mode.",
                    "int",
                    [](TerminalProfile const& p) {
                        return QVariant(static_cast<int>(unbox(p.modalCursorScrollOff.value())));
                    },
                    [](TerminalProfile& p, QVariant const& v) {
                        p.modalCursorScrollOff = LineCount(v.toInt());
                    } },
                  { "copy_last_mark_range_offset",
                    "Copy last-mark range offset",
                    "Line offset applied to the CopyPreviousMarkRange action.",
                    "int",
                    [](TerminalProfile const& p) {
                        return QVariant(static_cast<int>(unbox(p.copyLastMarkRangeOffset.value())));
                    },
                    [](TerminalProfile& p, QVariant const& v) {
                        p.copyLastMarkRangeOffset = LineOffset(v.toInt());
                    } },
              } },
            { "Behaviour",
              "⚙",
              {
                  boolField("escape_sandbox",
                            "Escape sandbox",
                            "Escape a Flatpak/Snap sandbox when spawning.",
                            [](auto& p) -> auto& { return p.escapeSandbox; }),
                  boolField("insert_after_yank",
                            "Insert after yank",
                            "Enter insert mode after yanking in Vi mode.",
                            [](auto& p) -> auto& { return p.insertAfterYank; }),
                  boolField("input_method_editor",
                            "Input method editor",
                            "Enable IME (input method) support.",
                            [](auto& p) -> auto& { return p.inputMethodEditor; }),
                  boolField("highlight_word_and_matches_on_double_click",
                            "Highlight word on double-click",
                            "Highlight the double-clicked word and its other occurrences.",
                            [](auto& p) -> auto& { return p.highlightDoubleClickedWord; }),
                  { "vi_mode_highlight_timeout",
                    "Vi highlight timeout (ms)",
                    "How long a Vi-mode yank highlight stays visible, in milliseconds.",
                    "int",
                    [](TerminalProfile const& p) {
                        return QVariant(static_cast<int>(p.highlightTimeout.value().count()));
                    },
                    [](TerminalProfile& p, QVariant const& v) { p.highlightTimeout = Ms { v.toInt() }; } },
                  { "screen_transition_duration",
                    "Screen transition (ms)",
                    "Duration of the alt-screen transition animation, in milliseconds.",
                    "int",
                    [](TerminalProfile const& p) {
                        return QVariant(static_cast<int>(p.screenTransitionDuration.value().count()));
                    },
                    [](TerminalProfile& p, QVariant const& v) {
                        p.screenTransitionDuration = Ms { v.toInt() };
                    } },
              } },
            { "Terminal",
              "▣",
              {
                  { "terminal_size_lines",
                    "Terminal rows",
                    "Default number of rows in the terminal grid.",
                    "int",
                    [](TerminalProfile const& p) {
                        return QVariant(static_cast<int>(unbox(p.terminalSize.value().lines)));
                    },
                    [](TerminalProfile& p, QVariant const& v) {
                        auto size = p.terminalSize.value();
                        size.lines = vtbackend::LineCount(v.toInt());
                        p.terminalSize = size;
                    } },
                  { "terminal_size_columns",
                    "Terminal columns",
                    "Default number of columns in the terminal grid.",
                    "int",
                    [](TerminalProfile const& p) {
                        return QVariant(static_cast<int>(unbox(p.terminalSize.value().columns)));
                    },
                    [](TerminalProfile& p, QVariant const& v) {
                        auto size = p.terminalSize.value();
                        size.columns = vtbackend::ColumnCount(v.toInt());
                        p.terminalSize = size;
                    } },
                  { "margins_horizontal",
                    "Horizontal margin (px)",
                    "Horizontal window margin in pixels.",
                    "int",
                    [](TerminalProfile const& p) {
                        return QVariant(static_cast<int>(*p.margins.value().horizontal));
                    },
                    [](TerminalProfile& p, QVariant const& v) {
                        auto m = p.margins.value();
                        m.horizontal = config::HorizontalMargin(static_cast<unsigned>(v.toInt()));
                        p.margins = m;
                    } },
                  { "margins_vertical",
                    "Vertical margin (px)",
                    "Vertical window margin in pixels.",
                    "int",
                    [](TerminalProfile const& p) {
                        return QVariant(static_cast<int>(*p.margins.value().vertical));
                    },
                    [](TerminalProfile& p, QVariant const& v) {
                        auto m = p.margins.value();
                        m.vertical = config::VerticalMargin(static_cast<unsigned>(v.toInt()));
                        p.margins = m;
                    } },
                  enumFieldFromTable<vtbackend::SearchCaseSensitivity>(
                      "search_case_sensitivity",
                      "Search case sensitivity",
                      "How the find bar compares letters when it opens. Its Aa button cycles the same "
                      "three at any time.",
                      [](TerminalProfile const& p) { return p.searchCaseSensitivity.value(); },
                      [](TerminalProfile& p, auto v) { p.searchCaseSensitivity = v; }),
                  enumField(
                      "pixel_reporting",
                      "Pixel reporting",
                      "How image dimensions are reported: logical pixels or device pixels.",
                      std::array {
                          std::pair { "logical"sv, config::PixelReporting::Logical },
                          std::pair { "device"sv, config::PixelReporting::Device },
                      },
                      [](TerminalProfile const& p) { return p.pixelReporting.value(); },
                      [](TerminalProfile& p, auto v) { p.pixelReporting = v; }),
                  enumField(
                      "terminal_id",
                      "Terminal ID",
                      "The VT terminal type to emulate.",
                      std::array {
                          std::pair { "vt100"sv, vtbackend::VTType::VT100 },
                          std::pair { "vt220"sv, vtbackend::VTType::VT220 },
                          std::pair { "vt240"sv, vtbackend::VTType::VT240 },
                          std::pair { "vt320"sv, vtbackend::VTType::VT320 },
                          std::pair { "vt330"sv, vtbackend::VTType::VT330 },
                          std::pair { "vt340"sv, vtbackend::VTType::VT340 },
                          std::pair { "vt420"sv, vtbackend::VTType::VT420 },
                          std::pair { "vt510"sv, vtbackend::VTType::VT510 },
                          std::pair { "vt520"sv, vtbackend::VTType::VT520 },
                          std::pair { "vt525"sv, vtbackend::VTType::VT525 },
                      },
                      [](TerminalProfile const& p) { return p.terminalId.value(); },
                      [](TerminalProfile& p, auto v) { p.terminalId = v; }),
                  boolField("option_key_as_alt",
                            "Option key as Alt",
                            "Treat the macOS Option key as Alt modifier.",
                            [](auto& p) -> auto& { return p.optionKeyAsAlt; }),
                  { "wm_class",
                    "WM class",
                    "The X11 WM_CLASS property string.",
                    "string",
                    [](TerminalProfile const& p) {
                        return QVariant(QString::fromStdString(p.wmClass.value()));
                    },
                    [](TerminalProfile& p, QVariant const& v) { p.wmClass = v.toString().toStdString(); } },
                  { "tab_label",
                    "Tab label",
                    "Template for the tab title (e.g. {WindowTitle}).",
                    "string",
                    [](TerminalProfile const& p) {
                        return QVariant(QString::fromStdString(p.tabLabel.value()));
                    },
                    [](TerminalProfile& p, QVariant const& v) { p.tabLabel = v.toString().toStdString(); } },
              } },
            { "History & scrollbar",
              "≡",
              {
                  historyLimitField("history_max_lines",
                                    "History max lines",
                                    "Maximum number of scrollback lines to keep (-1 = unlimited).",
                                    &config::HistoryConfig::maxHistoryLineCount),
                  historyLimitField(
                      "history_hard_limit",
                      "History hard limit",
                      "Upper bound on the scrollback (-1 = unlimited). Above the max, the extra "
                      "lines are headroom in which whole (prompt, output) blocks are evicted instead "
                      "of cutting mid-command. Values at or below the max disable that.",
                      &config::HistoryConfig::hardLimit),
                  { "history_scroll_multiplier",
                    "History scroll multiplier",
                    "Number of lines scrolled per scroll wheel step.",
                    "int",
                    [](TerminalProfile const& p) {
                        return QVariant(static_cast<int>(unbox(p.history.value().historyScrollMultiplier)));
                    },
                    [](TerminalProfile& p, QVariant const& v) {
                        auto h = p.history.value();
                        h.historyScrollMultiplier = vtbackend::LineCount(v.toInt());
                        p.history = h;
                    } },
                  { "auto_scroll_on_update",
                    "Auto-scroll on update",
                    "Scroll to the bottom automatically when new output arrives.",
                    "bool",
                    [](TerminalProfile const& p) { return QVariant(p.history.value().autoScrollOnUpdate); },
                    [](TerminalProfile& p, QVariant const& v) {
                        auto h = p.history.value();
                        h.autoScrollOnUpdate = v.toBool();
                        p.history = h;
                    } },
                  enumField(
                      "scrollbar_position",
                      "Scrollbar position",
                      "Where the scrollbar is shown.",
                      std::array {
                          std::pair { "hidden"sv, config::ScrollBarPosition::Hidden },
                          std::pair { "left"sv, config::ScrollBarPosition::Left },
                          std::pair { "right"sv, config::ScrollBarPosition::Right },
                      },
                      [](TerminalProfile const& p) { return p.scrollbar.value().position; },
                      [](TerminalProfile& p, auto v) {
                          auto sb = p.scrollbar.value();
                          sb.position = v;
                          p.scrollbar = sb;
                      }),
                  { "hide_scrollbar_in_alt_screen",
                    "Hide scrollbar in alt screen",
                    "Automatically hide the scrollbar when the application enters alternate screen mode.",
                    "bool",
                    [](TerminalProfile const& p) {
                        return QVariant(p.scrollbar.value().hideScrollbarInAltScreen);
                    },
                    [](TerminalProfile& p, QVariant const& v) {
                        auto sb = p.scrollbar.value();
                        sb.hideScrollbarInAltScreen = v.toBool();
                        p.scrollbar = sb;
                    } },
              } },
            { "Mouse & permissions",
              "⊙",
              {
                  { "hide_mouse_while_typing",
                    "Hide mouse while typing",
                    "Automatically hide the mouse cursor while typing.",
                    "bool",
                    [](TerminalProfile const& p) { return QVariant(p.mouse.value().hideWhileTyping); },
                    [](TerminalProfile& p, QVariant const& v) {
                        auto m = p.mouse.value();
                        m.hideWhileTyping = v.toBool();
                        p.mouse = m;
                    } },
                  enumField(
                      "permission_capture_buffer",
                      "Capture buffer permission",
                      "Permission required for the application to capture the screen buffer.",
                      std::array {
                          std::pair { "allow"sv, config::Permission::Allow },
                          std::pair { "deny"sv, config::Permission::Deny },
                          std::pair { "ask"sv, config::Permission::Ask },
                      },
                      [](TerminalProfile const& p) { return p.permissions.value().captureBuffer; },
                      [](TerminalProfile& p, auto v) {
                          auto perm = p.permissions.value();
                          perm.captureBuffer = v;
                          p.permissions = perm;
                      }),
                  enumField(
                      "permission_change_font",
                      "Change font permission",
                      "Permission required for the application to change the font.",
                      std::array {
                          std::pair { "allow"sv, config::Permission::Allow },
                          std::pair { "deny"sv, config::Permission::Deny },
                          std::pair { "ask"sv, config::Permission::Ask },
                      },
                      [](TerminalProfile const& p) { return p.permissions.value().changeFont; },
                      [](TerminalProfile& p, auto v) {
                          auto perm = p.permissions.value();
                          perm.changeFont = v;
                          p.permissions = perm;
                      }),
                  enumField(
                      "permission_display_statusline",
                      "Display statusline permission",
                      "Permission required for the application to show the host-writable status line.",
                      std::array {
                          std::pair { "allow"sv, config::Permission::Allow },
                          std::pair { "deny"sv, config::Permission::Deny },
                          std::pair { "ask"sv, config::Permission::Ask },
                      },
                      [](TerminalProfile const& p) {
                          return p.permissions.value().displayHostWritableStatusLine;
                      },
                      [](TerminalProfile& p, auto v) {
                          auto perm = p.permissions.value();
                          perm.displayHostWritableStatusLine = v;
                          p.permissions = perm;
                      }),
              } },
            { "Visual effects",
              "✦",
              {
                  enumField(
                      "blink_style",
                      "Blink style",
                      "How blinking text (SGR 5/6) is animated: classic toggle, smooth pulse, "
                      "or linger.",
                      std::array {
                          std::pair { "classic"sv, vtbackend::BlinkStyle::Classic },
                          std::pair { "smooth"sv, vtbackend::BlinkStyle::Smooth },
                          std::pair { "linger"sv, vtbackend::BlinkStyle::Linger },
                      },
                      [](TerminalProfile const& p) { return p.blinkStyle.value(); },
                      [](TerminalProfile& p, auto v) { p.blinkStyle = v; }),
                  enumField(
                      "screen_transition_style",
                      "Screen transition style",
                      "The visual animation when switching between primary and alternate screens.",
                      std::array {
                          std::pair { "classic"sv, vtbackend::ScreenTransitionStyle::Classic },
                          std::pair { "fade"sv, vtbackend::ScreenTransitionStyle::Fade },
                      },
                      [](TerminalProfile const& p) { return p.screenTransitionStyle.value(); },
                      [](TerminalProfile& p, auto v) { p.screenTransitionStyle = v; }),
                  { "cursor_motion_animation_duration",
                    "Cursor motion animation (ms)",
                    "Duration of the cursor motion animation, in milliseconds (0 = instant).",
                    "int",
                    [](TerminalProfile const& p) {
                        return QVariant(static_cast<int>(p.cursorMotionAnimationDuration.value().count()));
                    },
                    [](TerminalProfile& p, QVariant const& v) {
                        p.cursorMotionAnimationDuration = Ms { v.toInt() };
                    } },
              } },
            { "Cursor",
              "▮",
              {
                  { "insert_mode_cursor_blinking",
                    "Insert mode: blinking",
                    "Whether the cursor blinks in Insert mode.",
                    "bool",
                    [](TerminalProfile const& p) {
                        return QVariant(p.modeInsert.value().cursor.cursorDisplay
                                        == vtbackend::CursorDisplay::Blink);
                    },
                    [](TerminalProfile& p, QVariant const& v) {
                        auto mode = p.modeInsert.value();
                        mode.cursor.cursorDisplay =
                            v.toBool() ? vtbackend::CursorDisplay::Blink : vtbackend::CursorDisplay::Steady;
                        p.modeInsert = mode;
                    } },
                  enumField(
                      "insert_mode_cursor_shape",
                      "Insert mode: cursor shape",
                      "Cursor shape in Insert mode.",
                      CursorShapeNames,
                      [](TerminalProfile const& p) { return p.modeInsert.value().cursor.cursorShape; },
                      [](TerminalProfile& p, auto v) {
                          auto mode = p.modeInsert.value();
                          mode.cursor.cursorShape = v;
                          p.modeInsert = mode;
                      }),
                  { "insert_mode_cursor_blink_interval",
                    "Insert mode: blink interval (ms)",
                    "Cursor blink interval in Insert mode, in milliseconds.",
                    "int",
                    [](TerminalProfile const& p) {
                        return QVariant(
                            static_cast<int>(p.modeInsert.value().cursor.cursorBlinkInterval.count()));
                    },
                    [](TerminalProfile& p, QVariant const& v) {
                        auto mode = p.modeInsert.value();
                        mode.cursor.cursorBlinkInterval = Ms { v.toInt() };
                        p.modeInsert = mode;
                    } },
                  { "normal_mode_cursor_blinking",
                    "Normal mode: blinking",
                    "Whether the cursor blinks in Normal (Vi) mode.",
                    "bool",
                    [](TerminalProfile const& p) {
                        return QVariant(p.modeNormal.value().cursor.cursorDisplay
                                        == vtbackend::CursorDisplay::Blink);
                    },
                    [](TerminalProfile& p, QVariant const& v) {
                        auto mode = p.modeNormal.value();
                        mode.cursor.cursorDisplay =
                            v.toBool() ? vtbackend::CursorDisplay::Blink : vtbackend::CursorDisplay::Steady;
                        p.modeNormal = mode;
                    } },
                  enumField(
                      "normal_mode_cursor_shape",
                      "Normal mode: cursor shape",
                      "Cursor shape in Normal (Vi) mode.",
                      CursorShapeNames,
                      [](TerminalProfile const& p) { return p.modeNormal.value().cursor.cursorShape; },
                      [](TerminalProfile& p, auto v) {
                          auto mode = p.modeNormal.value();
                          mode.cursor.cursorShape = v;
                          p.modeNormal = mode;
                      }),
                  { "normal_mode_cursor_blink_interval",
                    "Normal mode: blink interval (ms)",
                    "Cursor blink interval in Normal (Vi) mode, in milliseconds.",
                    "int",
                    [](TerminalProfile const& p) {
                        return QVariant(
                            static_cast<int>(p.modeNormal.value().cursor.cursorBlinkInterval.count()));
                    },
                    [](TerminalProfile& p, QVariant const& v) {
                        auto mode = p.modeNormal.value();
                        mode.cursor.cursorBlinkInterval = Ms { v.toInt() };
                        p.modeNormal = mode;
                    } },
                  { "visual_mode_cursor_blinking",
                    "Visual mode: blinking",
                    "Whether the cursor blinks in Visual (Vi) mode.",
                    "bool",
                    [](TerminalProfile const& p) {
                        return QVariant(p.modeVisual.value().cursor.cursorDisplay
                                        == vtbackend::CursorDisplay::Blink);
                    },
                    [](TerminalProfile& p, QVariant const& v) {
                        auto mode = p.modeVisual.value();
                        mode.cursor.cursorDisplay =
                            v.toBool() ? vtbackend::CursorDisplay::Blink : vtbackend::CursorDisplay::Steady;
                        p.modeVisual = mode;
                    } },
                  enumField(
                      "visual_mode_cursor_shape",
                      "Visual mode: cursor shape",
                      "Cursor shape in Visual (Vi) mode.",
                      CursorShapeNames,
                      [](TerminalProfile const& p) { return p.modeVisual.value().cursor.cursorShape; },
                      [](TerminalProfile& p, auto v) {
                          auto mode = p.modeVisual.value();
                          mode.cursor.cursorShape = v;
                          p.modeVisual = mode;
                      }),
                  { "visual_mode_cursor_blink_interval",
                    "Visual mode: blink interval (ms)",
                    "Cursor blink interval in Visual (Vi) mode, in milliseconds.",
                    "int",
                    [](TerminalProfile const& p) {
                        return QVariant(
                            static_cast<int>(p.modeVisual.value().cursor.cursorBlinkInterval.count()));
                    },
                    [](TerminalProfile& p, QVariant const& v) {
                        auto mode = p.modeVisual.value();
                        mode.cursor.cursorBlinkInterval = Ms { v.toInt() };
                        p.modeVisual = mode;
                    } },
              } },
            { "Bell",
              "♪",
              {
                  { "bell_sound",
                    "Bell sound",
                    "Path to a sound file for the terminal bell, or 'default', or 'off'.",
                    "string",
                    [](TerminalProfile const& p) {
                        return QVariant(QString::fromStdString(p.bell.value().sound));
                    },
                    [](TerminalProfile& p, QVariant const& v) {
                        auto b = p.bell.value();
                        b.sound = v.toString().toStdString();
                        p.bell = b;
                    } },
                  { "bell_alert",
                    "Bell flash",
                    "Flash the terminal window when the bell rings.",
                    "bool",
                    [](TerminalProfile const& p) { return QVariant(p.bell.value().alert); },
                    [](TerminalProfile& p, QVariant const& v) {
                        auto b = p.bell.value();
                        b.alert = v.toBool();
                        p.bell = b;
                    } },
                  { "bell_volume",
                    "Bell volume",
                    "Volume of the bell sound (0.0 = silent, 1.0 = full).",
                    "double",
                    [](TerminalProfile const& p) {
                        return QVariant(static_cast<double>(p.bell.value().volume));
                    },
                    [](TerminalProfile& p, QVariant const& v) {
                        auto b = p.bell.value();
                        b.volume = static_cast<float>(v.toDouble());
                        p.bell = b;
                    } },
              } },
            { "Background",
              "▨",
              {
                  { "background_opacity",
                    "Background opacity",
                    "Terminal background opacity (0.0 = fully transparent, 1.0 = fully opaque).",
                    "double",
                    [](TerminalProfile const& p) {
                        return QVariant(
                            static_cast<double>(static_cast<uint8_t>(p.background.value().opacity)) / 255.0);
                    },
                    [](TerminalProfile& p, QVariant const& v) {
                        auto bg = p.background.value();
                        bg.opacity = vtbackend::Opacity(
                            static_cast<unsigned>(255 * std::clamp(v.toDouble(), 0.0, 1.0)));
                        p.background = bg;
                    } },
                  { "background_blur",
                    "Background blur",
                    "Whether to blur content behind the transparent terminal window (platform dependent).",
                    "bool",
                    [](TerminalProfile const& p) { return QVariant(p.background.value().blur); },
                    [](TerminalProfile& p, QVariant const& v) {
                        auto bg = p.background.value();
                        bg.blur = v.toBool();
                        p.background = bg;
                    } },
              } },
            { "Hyperlinks",
              "⧉",
              {
                  enumField(
                      "hyperlink_decoration_normal",
                      "Hyperlink decoration (normal)",
                      "The underline style for hyperlinks in their normal state.",
                      vtrasterizer::DecoratorNames,
                      [](TerminalProfile const& p) { return p.hyperlinkDecoration.value().normal; },
                      [](TerminalProfile& p, auto v) {
                          auto hd = p.hyperlinkDecoration.value();
                          hd.normal = v;
                          p.hyperlinkDecoration = hd;
                      }),
                  enumField(
                      "hyperlink_decoration_hover",
                      "Hyperlink decoration (hover)",
                      "The underline style for hyperlinks when hovered.",
                      vtrasterizer::DecoratorNames,
                      [](TerminalProfile const& p) { return p.hyperlinkDecoration.value().hover; },
                      [](TerminalProfile& p, auto v) {
                          auto hd = p.hyperlinkDecoration.value();
                          hd.hover = v;
                          p.hyperlinkDecoration = hd;
                      }),
              } },
            { "Status line",
              "▤",
              {
                  enumField(
                      "status_line_display",
                      "Status line display",
                      "Initial state of the status line: none (hidden) or indicator (shown).",
                      // HostWritable is deliberately absent: it is a state the host puts the
                      // terminal into at runtime, not an initial state a profile may request.
                      std::array {
                          std::pair { "none"sv, vtbackend::StatusDisplayType::None },
                          std::pair { "indicator"sv, vtbackend::StatusDisplayType::Indicator },
                      },
                      [](TerminalProfile const& p) { return p.statusLine.value().initialType; },
                      [](TerminalProfile& p, auto v) {
                          auto sl = p.statusLine.value();
                          sl.initialType = v;
                          p.statusLine = sl;
                      }),
                  enumField(
                      "status_line_position",
                      "Status line position",
                      "Where the status line appears: top or bottom of the terminal.",
                      std::array {
                          std::pair { "top"sv, vtbackend::StatusDisplayPosition::Top },
                          std::pair { "bottom"sv, vtbackend::StatusDisplayPosition::Bottom },
                      },
                      [](TerminalProfile const& p) { return p.statusLine.value().position; },
                      [](TerminalProfile& p, auto v) {
                          auto sl = p.statusLine.value();
                          sl.position = v;
                          p.statusLine = sl;
                      }),
                  { "status_line_sync_window_title",
                    "Sync window title with status line",
                    "Synchronize the window title with the host-writable status line content when denied.",
                    "bool",
                    [](TerminalProfile const& p) {
                        return QVariant(p.statusLine.value().syncWindowTitleWithHostWritableStatusDisplay);
                    },
                    [](TerminalProfile& p, QVariant const& v) {
                        auto sl = p.statusLine.value();
                        sl.syncWindowTitleWithHostWritableStatusDisplay = v.toBool();
                        p.statusLine = sl;
                    } },
              } },
        };
        return groups;
    }

    /// A data-driven descriptor for one editable color slot of a palette.
    struct SchemeColorDescriptor
    {
        QString key;
        QString label;
        std::function<vtbackend::RGBColor(vtbackend::ColorPalette const&)> get;
        std::function<void(vtbackend::ColorPalette&, vtbackend::RGBColor)> set;
    };

    /// The color slots the scheme editor exposes: the default fg/bg plus the 8 normal and 8 bright
    /// ANSI colors. Generated once from the ANSI names so the two 8-color banks are not hand-listed.
    std::vector<SchemeColorDescriptor> const& schemeColorDescriptors()
    {
        static auto const descriptors = [] {
            auto list = std::vector<SchemeColorDescriptor> {};
            list.push_back({ "background",
                             "Background",
                             [](auto const& p) { return p.defaultBackground; },
                             [](auto& p, auto c) { p.defaultBackground = c; } });
            list.push_back({ "foreground",
                             "Foreground",
                             [](auto const& p) { return p.defaultForeground; },
                             [](auto& p, auto c) { p.defaultForeground = c; } });
            static constexpr auto AnsiNames =
                std::array<std::string_view, 8> { "black", "red",     "green", "yellow",
                                                  "blue",  "magenta", "cyan",  "white" };
            for (auto const bright: { false, true })
                for (auto i = size_t { 0 }; i < AnsiNames.size(); ++i)
                {
                    auto const slot = bright ? i + 8 : i;
                    list.push_back({ QString::fromStdString((bright ? "bright_" : "normal_")
                                                            + std::string(AnsiNames[i])),
                                     QString::fromStdString((bright ? "Bright " : "Normal ")
                                                            + std::string(AnsiNames[i])),
                                     [slot](auto const& p) { return p.palette.at(slot); },
                                     [slot](auto& p, auto c) { p.palette.at(slot) = c; } });
                }
            return list;
        }();
        return descriptors;
    }

    /// A data-driven descriptor for one editable global (application-scope) setting. `toYaml` turns the
    /// edited value into the YAML scalar written to settings.yml; the load-time merge re-applies it
    /// through the typed per-key loader, so this side needs no parsing.
    struct GlobalFieldDescriptor
    {
        QString key;
        QString label;
        QString help;
        QString type;
        std::function<QVariant(config::Config const&)> get;
        std::function<std::string(QVariant const&)> toYaml;
        QStringList options {}; //!< For "enum": the allowed values (the combo model + accepted set).
    };

    QString boolToYaml(QVariant const& v)
    {
        return v.toBool() ? "true" : "false";
    }

    /// The editable global settings. The keys match contour.yml's top-level keys AND the loader lines in
    /// mergeGuiManagedSideFiles, so an override round-trips as the right type.
    std::vector<GlobalFieldDescriptor> const& globalFieldDescriptors()
    {
        static auto const descriptors = std::vector<GlobalFieldDescriptor> {
            { "reflow_on_resize",
              "Reflow on resize",
              "Reflow lines when the terminal is resized.",
              "bool",
              [](config::Config const& c) { return QVariant(c.reflowOnResize.value()); },
              [](QVariant const& v) { return boolToYaml(v).toStdString(); } },
            { "tab_switch_on_horizontal_wheel",
              "Switch tabs on horizontal wheel",
              "A horizontal wheel tilt or trackpad swipe switches to the previous/next tab.",
              "bool",
              [](config::Config const& c) { return QVariant(c.tabSwitchOnHorizontalWheel.value()); },
              [](QVariant const& v) { return boolToYaml(v).toStdString(); } },
            { "spawn_new_process",
              "Spawn new process",
              "Spawn a separate process for each new terminal window.",
              "bool",
              [](config::Config const& c) { return QVariant(c.spawnNewProcess.value()); },
              [](QVariant const& v) { return boolToYaml(v).toStdString(); } },
            { "read_buffer_size",
              "PTY read buffer size",
              "Number of bytes read from the pseudo-terminal per read.",
              "int",
              [](config::Config const& c) { return QVariant(c.ptyReadBufferSize.value()); },
              [](QVariant const& v) { return std::to_string(v.toInt()); } },
            { "command_palette_recent_count",
              "Recent commands",
              "How many recently-used commands the command palette lists first.",
              "int",
              [](config::Config const& c) { return QVariant(c.commandPaletteRecentCount.value()); },
              [](QVariant const& v) { return std::to_string(v.toInt()); } },
            { "word_delimiters",
              "Word delimiters",
              "Characters that separate words when selecting by double-click.",
              "string",
              [](config::Config const& c) {
                  return QVariant(QString::fromStdString(c.wordDelimiters.value()));
              },
              [](QVariant const& v) { return v.toString().toStdString(); } },
            { "theme",
              "GUI theme",
              "Light/dark appearance of the GUI chrome (menus, tabs, dialogs). The terminal keeps "
              "following the OS.",
              "enum",
              // Reuse the std::formatter<GuiTheme> (the "system"/"dark"/"light" source of truth) rather
              // than re-switching the enum here, matching the std::format idiom used for modifiers/keys.
              [](config::Config const& c) {
                  return QVariant(QString::fromStdString(std::format("{}", c.theme.value())));
              },
              [](QVariant const& v) { return v.toString().toStdString(); },
              { "system", "dark", "light" } },
            // Both tab bar rows read their options, their rendered value and their accepted set from
            // contour/TabBarMode.h, so the page cannot offer a token the configuration reader would
            // then reject -- and a new mode shows up here with this file untouched.
            { "tab_bar_position",
              "Tab bar position",
              "Where the tab strip sits relative to the terminal content.",
              "enum",
              [](config::Config const& c) {
                  return QVariant(toQString(config::configEnumToken(c.tabBarPosition.value())));
              },
              [](QVariant const& v) { return v.toString().toStdString(); },
              configEnumTokens<config::TabBarPosition>() },
            { "tab_bar_visibility",
              "Tab bar visibility",
              "When the tab strip is shown: always, never, or only once a window has more than one "
              "tab.",
              "enum",
              [](config::Config const& c) {
                  return QVariant(toQString(config::configEnumToken(c.tabBarVisibility.value())));
              },
              [](QVariant const& v) { return v.toString().toStdString(); },
              configEnumTokens<config::TabBarVisibility>() },
            // Reads its options and its accepted set from contour/UiStyle.h, like the tab bar rows
            // above. The restart note is part of the help text rather than a separate affordance
            // because the limitation is Qt's: the Quick Controls style is chosen once, before the
            // first control exists (see ContourGuiApp), so there is nothing to apply live.
            { "ui_style",
              "Interface style",
              "Native GUI chrome, or cell-quantized terminal (TUI) chrome drawn in a monospace font. "
              "Takes effect after restart.",
              "enum",
              [](config::Config const& c) {
                  return QVariant(toQString(config::configEnumToken(c.uiStyle.value())));
              },
              [](QVariant const& v) { return v.toString().toStdString(); },
              configEnumTokens<config::UiStyle>() },
            // No restart note, unlike the three ui_* rows around it: this one IS applied live (see
            // ContourGuiApp::applyWindowControlStyle), because nothing about it is pinned before the
            // first control exists -- it only decides what the title bar draws and where.
            { "window_control_style",
              "Window controls",
              "How the minimize/maximize/close buttons are drawn, and which side they sit on. Only "
              "applies while the native title bar is hidden.",
              "enum",
              [](config::Config const& c) {
                  return QVariant(toQString(config::configEnumToken(c.windowControlStyle.value())));
              },
              [](QVariant const& v) { return v.toString().toStdString(); },
              configEnumTokens<config::WindowControlStyle>() },
            // Same restart note as ui_style above, and for the same reason: the chrome's design
            // tokens are resolved from this font once, when the provider is built (ContourGuiApp),
            // and every property of it is CONSTANT. Saying so here is the whole difference between
            // "this setting needs a restart" and "this setting is broken".
            { "ui_font_family",
              "Interface font",
              "Font the terminal-style chrome is drawn with. Empty inherits the running profile's "
              "font, which is what makes the chrome match the terminal grid. Takes effect after "
              "restart.",
              "string",
              [](config::Config const& c) {
                  return QVariant(QString::fromStdString(c.uiFontFamily.value()));
              },
              [](QVariant const& v) { return v.toString().toStdString(); } },
            { "ui_font_size",
              "Interface font size",
              "Size, in points, the terminal-style chrome is drawn with. 0 inherits the running "
              "profile's font size. Takes effect after restart.",
              "double",
              [](config::Config const& c) { return QVariant(c.uiFontSize.value()); },
              [](QVariant const& v) { return std::format("{}", v.toDouble()); } },
            { "grapheme_clustering",
              "Grapheme clustering",
              "Whether DEC mode 2027 starts out set, letting a codepoint arriving after the first "
              "revise its grapheme cluster's width. Applications may still toggle it at runtime.",
              "bool",
              [](config::Config const& c) { return QVariant(c.graphemeClustering.value()); },
              [](QVariant const& v) { return v.toBool() ? "true" : "false"; } },
            { "text_scaling_method",
              "Scaled text quality",
              "How a glyph is enlarged when an application asks for scaled text. Stretch is free but "
              "softens at large sizes; rerasterize stays crisp at the cost of memory and rasterization.",
              "enum",
              // Reuse the std::formatter<GlyphScalingMethod>, which formats via nameOf() -- the same
              // source of truth the config reader parses, so the two cannot drift apart.
              [](config::Config const& c) {
                  return QVariant(QString::fromStdString(std::format("{}", c.textScalingMethod.value())));
              },
              [](QVariant const& v) { return v.toString().toStdString(); },
              { "stretch", "rerasterize" } },
        };
        return descriptors;
    }

    /// "#rrggbb" for @p color, the format the QML color pickers exchange.
    QString rgbToHex(vtbackend::RGBColor color)
    {
        return QString::asprintf("#%02x%02x%02x", color.red, color.green, color.blue);
    }

    /// Parses "#rrggbb" (or any Qt-recognized color string) into an RGBColor, black on a parse failure.
    vtbackend::RGBColor hexToRgb(QString const& hex)
    {
        auto const color = QColor(hex);
        return vtbackend::RGBColor { static_cast<uint8_t>(color.red()),
                                     static_cast<uint8_t>(color.green()),
                                     static_cast<uint8_t>(color.blue()) };
    }

    /// Formats a modifier chord as "Ctrl+Shift" (the individual Modifier has a formatter, the set does
    /// not), mirroring the config writer's bit walk.
    QString formatModifiers(vtbackend::Modifiers modifiers)
    {
        auto parts = QStringList {};
        for (auto bit = 0u; bit < sizeof(vtbackend::Modifier) * 8; ++bit)
        {
            auto const flag = static_cast<vtbackend::Modifier>(1u << bit);
            if (!modifiers.test(flag))
                continue;
            auto const name = std::format("{}", flag);
            if (!name.empty())
                parts << QString::fromStdString(name);
        }
        return parts.join('+');
    }

    /// Builds a read-only display row { trigger, action, mode } for one input binding, or an empty map
    /// when the binding carries no action.
    template <typename Binding>
    QVariantMap keybindingRow(Binding const& binding, QString const& inputLabel)
    {
        if (binding.binding.empty())
            return {};
        auto const mods = formatModifiers(binding.modifiers);
        auto row = QVariantMap {};
        row[QStringLiteral("trigger")] = mods.isEmpty() ? inputLabel : mods + '+' + inputLabel;
        row[QStringLiteral("action")] = QString::fromStdString(std::format("{}", binding.binding.front()));
        row[QStringLiteral("mode")] =
            binding.modes.any() ? QString::fromStdString(std::format("{}", binding.modes)) : QString {};
        return row;
    }

    /// The QML-facing token for a provenance value.
    QString originString(SettingsOrigin origin)
    {
        switch (origin)
        {
            case SettingsOrigin::Builtin: return QStringLiteral("builtin");
            case SettingsOrigin::MainConfig: return QStringLiteral("main");
            case SettingsOrigin::SideFile: return QStringLiteral("side");
        }
        return QStringLiteral("builtin");
    }
} // namespace

SettingsController::SettingsController(ConfigAccessor config,
                                       std::shared_ptr<config::GuiConfigStore> store,
                                       ApplyCallback apply,
                                       QObject* parent):
    QObject { parent },
    _config { std::move(config) },
    _store { std::move(store) },
    _apply { std::move(apply) }
{
    refresh();
}

SettingsOrigin SettingsController::profileOrigin(std::string const& name) const
{
    auto const& origins = _config().profileOrigins;
    auto const it = origins.find(name);
    return it != origins.end() ? it->second : SettingsOrigin::Builtin;
}

bool SettingsController::colorSchemeSideFileExists(std::string const& name) const
{
    auto ec = std::error_code {};
    auto const path = _config().configFile.parent_path() / "colorschemes" / (name + ".yml");
    return std::filesystem::exists(path, ec) && !ec;
}

bool SettingsController::isInlineColorScheme(std::string const& name) const
{
    // "Inline" means declared in contour.yml (origin MainConfig) with no GUI side file backing it. A side
    // file always wins editability regardless of a stale MainConfig marking (a side-file scheme referenced
    // by a profile is loaded into the config map and recorded MainConfig), so exclude it explicitly.
    auto const& origins = _config().colorSchemeOrigins;
    auto const it = origins.find(name);
    return it != origins.end() && it->second == SettingsOrigin::MainConfig
           && !colorSchemeSideFileExists(name);
}

bool SettingsController::fail(std::string const& error)
{
    emit errorOccurred(QString::fromStdString(error));
    return false;
}

void SettingsController::refresh()
{
    auto const& cfg = _config();
    _locked = cfg.guiConfigLocked.value();
    _defaultProfile = QString::fromStdString(cfg.defaultProfileName.value());

    // Profiles, name-sorted for a stable list.
    auto profileNames = std::vector<std::string> {};
    for (auto const& [name, _]: cfg.profiles.value())
        profileNames.push_back(name);
    std::ranges::sort(profileNames);

    _profiles.clear();
    for (auto const& name: profileNames)
    {
        auto const origin = profileOrigin(name);
        auto row = QVariantMap {};
        row[QStringLiteral("name")] = QString::fromStdString(name);
        row[QStringLiteral("origin")] = originString(origin);
        row[QStringLiteral("editable")] = (!_locked && origin == SettingsOrigin::SideFile);
        row[QStringLiteral("isDefault")] = (name == cfg.defaultProfileName.value());
        _profiles.push_back(row);
    }

    // Color schemes: those the config knows about, unioned with any colorschemes/*.yml on disk.
    auto schemeNames = std::set<std::string> {};
    for (auto const& [name, _]: cfg.colorschemes.value())
        schemeNames.insert(name);
    auto const schemesDir = cfg.configFile.parent_path() / "colorschemes";
    auto onDisk = std::set<std::string> {};
    auto ec = std::error_code {};
    if (std::filesystem::is_directory(schemesDir, ec))
        for (auto const& entry: std::filesystem::directory_iterator(schemesDir, ec))
            if (entry.is_regular_file() && entry.path().extension() == ".yml")
            {
                schemeNames.insert(entry.path().stem().string());
                onDisk.insert(entry.path().stem().string());
            }

    _colorSchemes.clear();
    for (auto const& name: schemeNames)
    {
        auto row = QVariantMap {};
        row[QStringLiteral("name")] = QString::fromStdString(name);
        row[QStringLiteral("editable")] = (!_locked && onDisk.contains(name));
        _colorSchemes.push_back(row);
    }

    // Keybindings (read-only): key, character and mouse mappings, each as { trigger, action, mode }.
    _keybindings.clear();
    auto const& mappings = cfg.inputMappings.value();
    for (auto const& binding: mappings.keyMappings)
        if (auto row = keybindingRow(binding, QString::fromStdString(std::format("{}", binding.input)));
            !row.isEmpty())
            _keybindings.push_back(row);
    for (auto const& binding: mappings.charMappings)
        if (auto row = keybindingRow(binding, QString(QChar(static_cast<char16_t>(binding.input))));
            !row.isEmpty())
            _keybindings.push_back(row);
    for (auto const& binding: mappings.mouseMappings)
        if (auto row = keybindingRow(binding, QString::fromStdString(std::format("{}", binding.input)));
            !row.isEmpty())
            _keybindings.push_back(row);

    emit changed();
}

// {{{ Profile draft

QVariantList SettingsController::profileFields() const
{
    auto fields = QVariantList {};
    if (!_hasDraft)
        return fields;
    // Flat, with each row carrying its group: the page groups the rows itself, and a flat list keeps a
    // key lookup (and every existing caller) working without a second traversal shape.
    for (auto const& group: profileFieldGroups())
        for (auto const& descriptor: group.fields)
        {
            auto row = QVariantMap {};
            row[QStringLiteral("key")] = descriptor.key;
            row[QStringLiteral("label")] = descriptor.label;
            row[QStringLiteral("help")] = descriptor.help;
            row[QStringLiteral("type")] = descriptor.type;
            row[QStringLiteral("value")] = descriptor.get(_draft);
            row[QStringLiteral("options")] = descriptor.options;
            row[QStringLiteral("group")] = group.title;
            row[QStringLiteral("groupGlyph")] = group.glyph;
            fields.push_back(row);
        }
    return fields;
}

void SettingsController::editProfile(QString const& name)
{
    auto const& cfg = _config();
    auto const* profile = cfg.findProfile(name.toStdString());
    if (profile == nullptr)
        return;
    _draft = *profile;
    _editingProfile = name;
    _hasDraft = true;
    _editingReadOnly = _locked || profileOrigin(name.toStdString()) != SettingsOrigin::SideFile;
    emit draftChanged();
}

void SettingsController::newProfile(QString const& basedOn)
{
    auto const& cfg = _config();
    auto const* base = cfg.findProfile(basedOn.toStdString());
    if (base == nullptr)
        base = cfg.findProfile(cfg.defaultProfileName.value());
    _draft = base != nullptr ? *base : TerminalProfile {};
    _editingProfile.clear(); // unsaved: needs Save As
    _hasDraft = true;
    _editingReadOnly = _locked;
    emit draftChanged();
}

void SettingsController::setProfileField(QString const& key, QVariant const& value)
{
    if (!_hasDraft)
        return;
    for (auto const& group: profileFieldGroups())
        for (auto const& descriptor: group.fields)
            if (descriptor.key == key)
            {
                descriptor.set(_draft, value);
                emit draftChanged();
                return;
            }
}

QString SettingsController::colorSchemeMode() const
{
    if (_hasDraft && std::holds_alternative<config::DualColorConfig>(_draft.colors.value()))
        return QStringLiteral("dual");
    return QStringLiteral("simple");
}

QString SettingsController::colorScheme() const
{
    if (_hasDraft)
        if (auto const* simple = std::get_if<config::SimpleColorConfig>(&_draft.colors.value()))
            return QString::fromStdString(simple->colorScheme);
    return {};
}

QString SettingsController::colorSchemeLight() const
{
    if (_hasDraft)
        if (auto const* dual = std::get_if<config::DualColorConfig>(&_draft.colors.value()))
            return QString::fromStdString(dual->colorSchemeLight);
    return {};
}

QString SettingsController::colorSchemeDark() const
{
    if (_hasDraft)
        if (auto const* dual = std::get_if<config::DualColorConfig>(&_draft.colors.value()))
            return QString::fromStdString(dual->colorSchemeDark);
    return {};
}

void SettingsController::setColorSchemeMode(QString const& mode)
{
    if (!_hasDraft)
        return;
    auto& colors = _draft.colors.value();
    if (mode == QStringLiteral("dual") && std::holds_alternative<config::SimpleColorConfig>(colors))
    {
        auto const name = std::get<config::SimpleColorConfig>(colors).colorScheme;
        colors = config::DualColorConfig { .colorSchemeLight = name, .colorSchemeDark = name };
        emit draftChanged();
    }
    else if (mode == QStringLiteral("simple") && std::holds_alternative<config::DualColorConfig>(colors))
    {
        auto const name = std::get<config::DualColorConfig>(colors).colorSchemeDark;
        colors = config::SimpleColorConfig { .colorScheme = name };
        emit draftChanged();
    }
}

void SettingsController::setColorScheme(QString const& name)
{
    if (!_hasDraft)
        return;
    if (auto* simple = std::get_if<config::SimpleColorConfig>(&_draft.colors.value()))
    {
        simple->colorScheme = name.toStdString();
        emit draftChanged();
    }
}

void SettingsController::setColorSchemeLight(QString const& name)
{
    if (!_hasDraft)
        return;
    if (auto* dual = std::get_if<config::DualColorConfig>(&_draft.colors.value()))
    {
        dual->colorSchemeLight = name.toStdString();
        emit draftChanged();
    }
}

void SettingsController::setColorSchemeDark(QString const& name)
{
    if (!_hasDraft)
        return;
    if (auto* dual = std::get_if<config::DualColorConfig>(&_draft.colors.value()))
    {
        dual->colorSchemeDark = name.toStdString();
        emit draftChanged();
    }
}

bool SettingsController::saveProfile()
{
    if (!_hasDraft || _locked)
        return fail("The settings page is read-only.");
    if (_editingProfile.isEmpty())
        return fail("This profile has no name yet — use \"Save As\".");
    if (_editingReadOnly)
        return fail("This profile is defined in contour.yml and cannot be overwritten — use \"Save As\".");

    if (auto const result = _store->saveProfile(_editingProfile.toStdString(), _draft); !result)
        return fail(result.error());

    _apply();
    refresh();
    editProfile(_editingProfile); // re-seed from the reloaded config
    return true;
}

bool SettingsController::saveProfileAs(QString const& newName)
{
    if (!_hasDraft || _locked)
        return fail("The settings page is read-only.");
    auto const trimmed = newName.trimmed();
    if (trimmed.isEmpty())
        return fail("Enter a name for the new profile.");
    // Reject ANY existing name, not just contour.yml ones: an existing GUI (side-file) profile of the
    // same name would otherwise be silently overwritten with the current draft. findProfile covers both
    // inline and side-file profiles, matching renameProfile's collision guard.
    if (_config().findProfile(trimmed.toStdString()) != nullptr)
        return fail("A profile with that name already exists; choose another name.");

    if (auto const result = _store->saveProfile(trimmed.toStdString(), _draft); !result)
        return fail(result.error());

    _apply();
    refresh();
    editProfile(trimmed);
    return true;
}

bool SettingsController::deleteProfile(QString const& name)
{
    if (_locked)
        return fail("The settings page is read-only.");
    if (profileOrigin(name.toStdString()) != SettingsOrigin::SideFile)
        return fail("Only GUI-created profiles can be deleted here.");

    if (auto const result = _store->deleteProfile(name.toStdString()); !result)
        return fail(result.error());

    if (_editingProfile == name)
    {
        _hasDraft = false;
        _editingProfile.clear();
        emit draftChanged();
    }
    _apply();
    refresh();
    return true;
}

// }}}

bool SettingsController::renameProfile(QString const& oldName, QString const& newName)
{
    if (_locked)
        return fail("The settings page is read-only.");
    if (profileOrigin(oldName.toStdString()) != SettingsOrigin::SideFile)
        return fail("Only GUI-created profiles can be renamed here.");
    auto const trimmed = newName.trimmed();
    if (trimmed.isEmpty())
        return fail("Enter a name for the profile.");
    if (trimmed == oldName)
        return true; // nothing to do
    if (_config().findProfile(trimmed.toStdString()) != nullptr)
        return fail("A profile with that name already exists.");

    // Copy the profile out: findProfile's pointer is invalidated by _apply()'s in-place config reload.
    auto const* profile = _config().findProfile(oldName.toStdString());
    if (profile == nullptr)
        return fail("Profile not found.");
    auto const profileCopy = *profile;
    bool const wasDefault = (_defaultProfile == oldName);

    if (auto const result = _store->saveProfile(trimmed.toStdString(), profileCopy); !result)
        return fail(result.error());
    if (auto const result = _store->deleteProfile(oldName.toStdString()); !result)
        return fail(result.error());
    if (wasDefault)
    {
        auto settings = _config().guiManagedSettings;
        settings.defaultProfile = trimmed.toStdString();
        if (auto const result = _store->saveGuiSettings(settings); !result)
            return fail(result.error());
    }

    _apply();
    refresh();
    if (_editingProfile == oldName)
        editProfile(trimmed);
    return true;
}

bool SettingsController::setDefaultProfile(QString const& name)
{
    if (_locked)
        return fail("The settings page is read-only.");

    auto settings = _config().guiManagedSettings;
    settings.defaultProfile = name.toStdString();
    if (auto const result = _store->saveGuiSettings(settings); !result)
        return fail(result.error());

    _apply();
    refresh();
    return true;
}

QVariantList SettingsController::globalFields() const
{
    auto const& cfg = _config();
    auto const& overrides = cfg.guiManagedSettings.globalOverrides;
    auto fields = QVariantList {};
    for (auto const& descriptor: globalFieldDescriptors())
    {
        auto row = QVariantMap {};
        row[QStringLiteral("key")] = descriptor.key;
        row[QStringLiteral("label")] = descriptor.label;
        row[QStringLiteral("help")] = descriptor.help;
        row[QStringLiteral("type")] = descriptor.type;
        row[QStringLiteral("value")] = descriptor.get(cfg);
        row[QStringLiteral("options")] = descriptor.options;
        row[QStringLiteral("overridden")] = overrides.contains(descriptor.key.toStdString());
        fields.push_back(row);
    }
    return fields;
}

bool SettingsController::setGlobalField(QString const& key, QVariant const& value)
{
    if (_locked)
        return fail("The settings page is read-only.");
    for (auto const& descriptor: globalFieldDescriptors())
        if (descriptor.key == key)
        {
            auto settings = _config().guiManagedSettings;
            settings.globalOverrides[key.toStdString()] = descriptor.toYaml(value);
            if (auto const result = _store->saveGuiSettings(settings); !result)
                return fail(result.error());
            _apply();
            refresh();
            return true;
        }
    return fail("Unknown global setting.");
}

bool SettingsController::resetGlobalField(QString const& key)
{
    if (_locked)
        return fail("The settings page is read-only.");
    auto settings = _config().guiManagedSettings;
    settings.globalOverrides.erase(key.toStdString());
    if (auto const result = _store->saveGuiSettings(settings); !result)
        return fail(result.error());
    _apply();
    refresh();
    return true;
}

// {{{ Color-scheme draft

QVariantList SettingsController::schemeColors() const
{
    auto colors = QVariantList {};
    if (!_hasSchemeDraft)
        return colors;
    for (auto const& descriptor: schemeColorDescriptors())
    {
        auto row = QVariantMap {};
        row[QStringLiteral("key")] = descriptor.key;
        row[QStringLiteral("label")] = descriptor.label;
        row[QStringLiteral("color")] = rgbToHex(descriptor.get(_schemeDraft));
        colors.push_back(row);
    }
    return colors;
}

void SettingsController::editColorScheme(QString const& name)
{
    auto const& cfg = _config();
    auto palette = vtbackend::ColorPalette {};
    if (auto const it = cfg.colorschemes.value().find(name.toStdString());
        it != cfg.colorschemes.value().end())
        palette = it->second;
    else if (auto const loaded = config::loadColorSchemeFile(cfg.configFile.parent_path() / "colorschemes"
                                                             / (name.toStdString() + ".yml")))
        palette = *loaded;

    _schemeDraft = palette;
    _editingScheme = name;
    _hasSchemeDraft = true;
    emit schemeDraftChanged();
    emit editingSchemeChanged();
}

void SettingsController::newColorScheme(QString const& basedOn)
{
    auto const& cfg = _config();
    auto palette = vtbackend::ColorPalette {};
    if (auto const it = cfg.colorschemes.value().find(basedOn.toStdString());
        it != cfg.colorschemes.value().end())
        palette = it->second;

    _schemeDraft = palette;
    _editingScheme.clear();
    _hasSchemeDraft = true;
    emit schemeDraftChanged();
    emit editingSchemeChanged();
}

void SettingsController::setSchemeColor(QString const& key, QString const& color)
{
    if (!_hasSchemeDraft)
        return;
    for (auto const& descriptor: schemeColorDescriptors())
        if (descriptor.key == key)
        {
            descriptor.set(_schemeDraft, hexToRgb(color));
            emit schemeDraftChanged();
            return;
        }
}

bool SettingsController::renameColorScheme(QString const& oldName, QString const& newName)
{
    if (_locked)
        return fail("The settings page is read-only.");
    auto const schemesDir = _config().configFile.parent_path() / "colorschemes";
    if (!colorSchemeSideFileExists(oldName.toStdString()))
        return fail("Only GUI-created color schemes can be renamed here.");
    auto const trimmed = newName.trimmed();
    if (trimmed.isEmpty())
        return fail("Enter a name for the color scheme.");
    if (trimmed == oldName)
        return true; // nothing to do
    // Reject a destination naming ANY existing scheme: a side file, a contour.yml inline scheme (which
    // would shadow the written side file at load, leaving a dead file), or a config-known scheme such as
    // the builtin "default". Mirrors renameProfile's guard, and uses the non-throwing exists() overload
    // (via the helper) so an I/O error cannot throw out of a slot.
    if (colorSchemeSideFileExists(trimmed.toStdString()) || isInlineColorScheme(trimmed.toStdString())
        || _config().colorschemes.value().contains(trimmed.toStdString()))
        return fail("A color scheme with that name already exists.");

    // Load the old palette (config-known or from its side file), then write it under the new name.
    auto palette = vtbackend::ColorPalette {};
    if (auto const it = _config().colorschemes.value().find(oldName.toStdString());
        it != _config().colorschemes.value().end())
        palette = it->second;
    else if (auto const loaded = config::loadColorSchemeFile(schemesDir / (oldName.toStdString() + ".yml")))
        palette = *loaded;

    if (auto const result = _store->saveColorScheme(trimmed.toStdString(), palette); !result)
        return fail(result.error());
    if (auto const result = _store->deleteColorScheme(oldName.toStdString()); !result)
        return fail(result.error());

    _apply();
    refresh();
    if (_editingScheme == oldName)
        editColorScheme(trimmed);
    return true;
}

bool SettingsController::saveColorScheme(QString const& name)
{
    if (!_hasSchemeDraft || _locked)
        return fail("The settings page is read-only.");
    if (name.trimmed().isEmpty())
        return fail("Enter a name for the color scheme.");
    // A contour.yml inline scheme of the same name shadows a side file at load (the inline node wins), so
    // writing one would silently have no effect. Refuse it, mirroring saveProfileAs's contour.yml guard.
    if (isInlineColorScheme(name.trimmed().toStdString()))
        return fail("A color scheme with that name is defined in contour.yml; choose another name.");

    if (auto const result = _store->saveColorScheme(name.toStdString(), _schemeDraft); !result)
        return fail(result.error());

    _apply();
    refresh();
    _editingScheme = name;
    emit schemeDraftChanged();
    emit editingSchemeChanged();
    return true;
}

bool SettingsController::deleteColorScheme(QString const& name)
{
    if (_locked)
        return fail("The settings page is read-only.");
    // Only GUI-created (side-file) schemes can be deleted; a builtin/inline scheme has no side file, and
    // removeFile treats an absent file as success, so without this guard we would report a false delete of
    // a scheme that is actually still present. Mirrors deleteProfile's origin guard.
    if (!colorSchemeSideFileExists(name.toStdString()))
        return fail("Only GUI-created color schemes can be deleted here.");

    if (auto const result = _store->deleteColorScheme(name.toStdString()); !result)
        return fail(result.error());

    if (_editingScheme == name)
    {
        _hasSchemeDraft = false;
        _editingScheme.clear();
        emit schemeDraftChanged();
        emit editingSchemeChanged();
    }
    _apply();
    refresh();
    return true;
}

// }}}

// {{{ Indicator status line visual editor bridge

// The settings page edits the indicator status line as a list of items rather than as a raw template
// string. These four methods are the whole bridge, and all of them drive off
// vtbackend::StatusLineDefinitions::ItemTraits and ::CellFlagNames, so the placeholders the page offers
// and the flags it can toggle are exactly the ones the template parser understands. Both used to be
// re-listed here -- the parse side as seventeen near-identical visitor overloads that exposed four of
// the thirteen flags, the serialize side as a seventeen-branch name chain that rebuilt Tabs with its
// active colours discarded -- so a profile could lose styling merely by being opened and saved.

namespace
{
    /// The QVariantMap key each cell flag is exposed under, i.e. its template spelling.
    [[nodiscard]] QString flagKey(vtbackend::StatusLineDefinitions::CellFlagName const& flag)
    {
        return toQString(flag.name);
    }

    /// Exposes an optional colour as the pair of keys the editor reads: a bool saying whether it is set
    /// at all, and its "#RRGGBB" text. Split in two because an unset colour is not black -- it means
    /// "inherit" -- and one string cannot say both.
    void writeColorPair(QVariantMap& row,
                        QString const& hasKey,
                        QString const& colorKey,
                        std::optional<vtbackend::RGBColor> const& color)
    {
        row[hasKey] = color.has_value();
        row[colorKey] = color ? QString::fromStdString(vtbackend::formatColor(*color)) : QString {};
    }

    /// Reads the styles shared by every item into @p row.
    void readStyles(QVariantMap& row, vtbackend::StatusLineDefinitions::Styles const& styles)
    {
        writeColorPair(row, QStringLiteral("hasColor"), QStringLiteral("color"), styles.foregroundColor);
        writeColorPair(row, QStringLiteral("hasBgColor"), QStringLiteral("bgColor"), styles.backgroundColor);

        auto flags = QVariantMap {};
        for (auto const& flag: vtbackend::StatusLineDefinitions::CellFlagNames)
            flags[flagKey(flag)] = styles.flags.test(flag.flag);
        row[QStringLiteral("flags")] = flags;

        row[QStringLiteral("textLeft")] = QString::fromStdString(styles.textLeft);
        row[QStringLiteral("textRight")] = QString::fromStdString(styles.textRight);
    }

    /// Writes the styles shared by every item back out of @p row.
    [[nodiscard]] vtbackend::StatusLineDefinitions::Styles writeStyles(QVariantMap const& row)
    {
        auto styles = vtbackend::StatusLineDefinitions::Styles {};

        if (row.value(QStringLiteral("hasColor")).toBool())
            styles.foregroundColor = hexToRgb(row.value(QStringLiteral("color")).toString());
        if (row.value(QStringLiteral("hasBgColor")).toBool())
            styles.backgroundColor = hexToRgb(row.value(QStringLiteral("bgColor")).toString());

        auto const flags = row.value(QStringLiteral("flags")).toMap();
        for (auto const& flag: vtbackend::StatusLineDefinitions::CellFlagNames)
            if (flags.value(flagKey(flag)).toBool())
                styles.flags.enable(flag.flag);

        styles.textLeft = row.value(QStringLiteral("textLeft")).toString().toStdString();
        styles.textRight = row.value(QStringLiteral("textRight")).toString().toStdString();
        return styles;
    }

    /// An optional colour from the "has"/value key pair @p hasKey / @p colorKey.
    [[nodiscard]] std::optional<vtbackend::RGBColor> optionalColor(QVariantMap const& row,
                                                                   QString const& hasKey,
                                                                   QString const& colorKey)
    {
        if (!row.value(hasKey).toBool())
            return std::nullopt;
        return hexToRgb(row.value(colorKey).toString());
    }
} // namespace

QVariantList SettingsController::parseIndicatorSegment(QString const& templateStr) const
{
    using namespace vtbackend::StatusLineDefinitions;

    auto items = QVariantList {};
    for (auto const& item: vtbackend::parseStatusLineSegment(templateStr.toStdString()))
    {
        std::visit(
            [&items](auto const& v) {
                using T = std::decay_t<decltype(v)>;

                auto row = QVariantMap {};
                readStyles(row, v);
                row[QStringLiteral("type")] = toQString(ItemTraits<T>::Name);
                row[QStringLiteral("label")] = toQString(ItemTraits<T>::Label);
                row[QStringLiteral("sample")] = toQString(ItemTraits<T>::Sample);

                // The alternatives carrying payload beyond Styles. Everything the editor can change has
                // to appear here, or serializeIndicatorSegment cannot put it back.
                if constexpr (std::same_as<T, Text>)
                {
                    row[QStringLiteral("text")] = QString::fromStdString(v.text);
                    row[QStringLiteral("displayName")] =
                        v.text.empty() ? toQString(ItemTraits<T>::Label) : QString::fromStdString(v.text);
                }
                else if constexpr (std::same_as<T, Command>)
                {
                    row[QStringLiteral("command")] = QString::fromStdString(v.command);
                    row[QStringLiteral("displayName")] = v.command.empty()
                                                             ? toQString(ItemTraits<T>::Label)
                                                             : QString::fromStdString(v.command);
                }
                else if constexpr (std::same_as<T, Tabs>)
                {
                    writeColorPair(
                        row, QStringLiteral("hasActiveColor"), QStringLiteral("activeColor"), v.activeColor);
                    writeColorPair(row,
                                   QStringLiteral("hasActiveBackground"),
                                   QStringLiteral("activeBackground"),
                                   v.activeBackground);
                    row[QStringLiteral("separator")] =
                        QString::fromStdString(v.separator.value_or(std::string {}));
                    row[QStringLiteral("displayName")] = toQString(ItemTraits<T>::Label);
                }
                else if constexpr (std::same_as<T, Context>)
                {
                    // Carried through even though the page offers no control for them yet: an item is
                    // round-tripped through here on EVERY edit of the segment, so a payload the reader
                    // drops is a payload the writer silently deletes from the user's profile.
                    row[QStringLiteral("verbosity")] = toQString(contextVerbosityName(v.verbosity));
                    row[QStringLiteral("separator")] =
                        QString::fromStdString(v.separator.value_or(std::string {}));
                    row[QStringLiteral("maxWidth")] = unbox<int>(v.maxWidth);
                    row[QStringLiteral("displayName")] = toQString(ItemTraits<T>::Label);
                }
                else
                    row[QStringLiteral("displayName")] = toQString(ItemTraits<T>::Label);

                items.push_back(row);
            },
            item);
    }
    return items;
}

QString SettingsController::serializeIndicatorSegment(QVariantList const& items) const
{
    using namespace vtbackend::StatusLineDefinitions;

    auto segment = vtbackend::StatusLineSegment {};
    for (auto const& raw: items)
    {
        auto const row = raw.toMap();
        auto const typeName = row.value(QStringLiteral("type")).toString();

        // Matched against ItemTraits rather than a chain of string literals, so the names accepted here
        // are the names parseIndicatorSegment produces. An unknown name is skipped: the page cannot
        // author one, and inventing an item for it would put text into the user's profile.
        forEachItemType([&](auto tag) {
            using T = decltype(tag)::type;
            if (typeName != toQString(ItemTraits<T>::Name))
                return;

            auto const styles = writeStyles(row);
            if constexpr (std::same_as<T, Text>)
                segment.emplace_back(
                    Text { styles, row.value(QStringLiteral("text")).toString().toStdString() });
            else if constexpr (std::same_as<T, Command>)
                segment.emplace_back(
                    Command { styles, row.value(QStringLiteral("command")).toString().toStdString() });
            else if constexpr (std::same_as<T, Tabs>)
            {
                auto const separator = row.value(QStringLiteral("separator")).toString();
                segment.emplace_back(Tabs {
                    styles,
                    optionalColor(row, QStringLiteral("hasActiveColor"), QStringLiteral("activeColor")),
                    optionalColor(
                        row, QStringLiteral("hasActiveBackground"), QStringLiteral("activeBackground")),
                    // An empty separator is "unset", so Tabs falls back to its built-in "|" rather than
                    // rendering the tabs run together.
                    separator.isEmpty() ? std::nullopt : std::optional { separator.toStdString() },
                });
            }
            else if constexpr (std::same_as<T, Context>)
            {
                auto item = Context { styles };
                item.verbosity =
                    contextVerbosityFrom(row.value(QStringLiteral("verbosity")).toString().toStdString());
                auto const separator = row.value(QStringLiteral("separator")).toString();
                if (!separator.isEmpty())
                    item.separator = separator.toStdString();
                // A missing or nonsensical width keeps the default rather than collapsing the
                // breadcrumb to nothing.
                if (auto const width = row.value(QStringLiteral("maxWidth")).toInt(); width > 0)
                    item.maxWidth = vtbackend::ColumnCount(width);
                segment.emplace_back(std::move(item));
            }
            else
                segment.emplace_back(T { styles });
        });
    }

    return QString::fromStdString(vtbackend::serializeStatusLineSegment(segment));
}

QVariantList SettingsController::indicatorPlaceholders() const
{
    using namespace vtbackend::StatusLineDefinitions;

    auto catalog = QVariantList {};
    forEachItemType([&catalog](auto tag) {
        using T = decltype(tag)::type;
        auto row = QVariantMap {};
        row[QStringLiteral("type")] = toQString(ItemTraits<T>::Name);
        row[QStringLiteral("label")] = toQString(ItemTraits<T>::Label);
        row[QStringLiteral("sample")] = toQString(ItemTraits<T>::Sample);
        catalog.push_back(row);
    });
    return catalog;
}

QVariantList SettingsController::indicatorFlags() const
{
    auto catalog = QVariantList {};
    for (auto const& flag: vtbackend::StatusLineDefinitions::CellFlagNames)
    {
        auto row = QVariantMap {};
        row[QStringLiteral("key")] = toQString(flag.name);
        row[QStringLiteral("label")] = toQString(flag.label);
        catalog.push_back(row);
    }
    return catalog;
}

QString SettingsController::indicatorSegment(int segmentIndex) const
{
    if (!_hasDraft)
        return {};
    auto const& indicator = _draft.statusLine.value().indicator;
    switch (segmentIndex)
    {
        case 0: return QString::fromStdString(indicator.left);
        case 1: return QString::fromStdString(indicator.middle);
        case 2: return QString::fromStdString(indicator.right);
        default: return {};
    }
}

void SettingsController::setIndicatorSegment(int segmentIndex, QString const& value)
{
    if (!_hasDraft || _editingReadOnly)
        return;

    auto statusLine = _draft.statusLine.value();
    switch (segmentIndex)
    {
        case 0: statusLine.indicator.left = value.toStdString(); break;
        case 1: statusLine.indicator.middle = value.toStdString(); break;
        case 2: statusLine.indicator.right = value.toStdString(); break;
        default: return;
    }
    _draft.statusLine = statusLine;
    emit draftChanged();
}

// }}}

} // namespace contour::window
