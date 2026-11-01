// SPDX-License-Identifier: Apache-2.0
#include <contour/command/Shortcut.hpp>
#include <contour/config/FileOpenerTemplate.hpp>
#include <contour/config/GlobalOverrides.hpp>
#include <contour/config/HostSideFile.hpp>
#include <contour/display/ShortcutOverride.hpp>
#include <contour/platform/HotkeyQtKeys.hpp>
#include <contour/platform/QtPath.hpp>
#include <contour/window/ColorContrastReadout.hpp>
#include <contour/window/HotkeyRefusals.hpp>
#include <contour/window/NoticeQueue.hpp>
#include <contour/window/ProfileRules.hpp>
#include <contour/window/SettingsController.hpp>
#include <contour/window/SettingsControls.hpp>
#include <contour/window/SettingsFields.hpp>
#include <contour/window/SettingsHostFields.hpp>
#include <contour/window/SettingsSearch.hpp>
#include <contour/window/SettingsSections.hpp>
#include <contour/window/ShellInstallRows.hpp>
#include <contour/window/ShellIntegrationOffer.hpp>

#include <vtbackend/core/ColorPalette.hpp>
#include <vtbackend/core/CommandBlockOutcome.hpp>
#include <vtbackend/links/LinkMatcher.hpp>
#include <vtbackend/links/LinkPatterns.hpp>
#include <vtbackend/links/ScanText.hpp>
#include <vtbackend/screen/GutterSettings.hpp>
#include <vtbackend/screen/StatusLineBuilder.hpp>
#include <vtbackend/shell/Folding.hpp>
#include <vtbackend/shell/ScrollbarMarks.hpp>
#include <vtbackend/shell/StickyHeader.hpp>

#include <text_shaper/Font.hpp>

#include <core/Assert.hpp>
#include <core/Utf8Path.hpp>
#include <core/log/LogStore.hpp>

#include <yaml-cpp/yaml.h>

#include <QtCore/QStringList>
#include <QtGui/QColor>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace contour::window
{

using config::SettingsOrigin;
using config::TerminalProfile;
using namespace std::string_view_literals;
using fields::controlOf;
using fields::GlobalFieldDescriptor;
using fields::globalFieldDescriptors;
using fields::OptionContext;
using fields::profileFieldGroups;
using fields::unboundedRange;

namespace
{
    /// The row's key the quick terminal's hotkey is edited under.
    constexpr auto HotkeyKey = QLatin1StringView("quick_terminal.hotkey");

    /// The page's background: what most of a scheme's colours are drawn on.
    [[nodiscard]] vtbackend::RGBColor pageBackground(vtbackend::ColorPalette const& palette)
    {
        return palette.defaultBackground;
    }

    /// A colour slot a scheme may leave unset, in which case the palette derives it. The editor row
    /// shows the colour in effect and writes an explicit one only when edited.
    struct DerivedColorSlot
    {
        std::string_view key;   //!< QML key.
        std::string_view label; //!< Swatch label.
        /// The scheme's own colour for the slot; unset while the palette derives it.
        std::optional<vtbackend::RGBColor> (*own)(vtbackend::ColorPalette const&);
        /// Sets the scheme's own colour, or unsets it so that the palette derives it again.
        void (*assign)(vtbackend::ColorPalette&, std::optional<vtbackend::RGBColor>);
        /// The colour in effect: the scheme's own, or the derived one while the slot is unset.
        vtbackend::RGBColor (*resolve)(vtbackend::ColorPalette const&);
        /// The colour the slot is drawn on, which its contrast readout is measured against.
        vtbackend::RGBColor (*sitsOn)(vtbackend::ColorPalette const&) = &pageBackground;
    };

    /// The slot of the block column's colour for @p Outcome in one state: the palette's array @p Colors,
    /// resolved by @p Resolve.
    /// @param key   QML key.
    /// @param label Swatch label.
    /// @return The slot.
    template <auto Colors, auto Resolve, vtbackend::CommandBlockOutcome Outcome>
    constexpr DerivedColorSlot outcomeSlot(std::string_view key, std::string_view label)
    {
        constexpr auto Index = static_cast<size_t>(Outcome);
        return { .key = key,
                 .label = label,
                 .own = [](vtbackend::ColorPalette const& p) { return (p.*Colors).at(Index); },
                 .assign = [](vtbackend::ColorPalette& p,
                              std::optional<vtbackend::RGBColor> color) { (p.*Colors).at(Index) = color; },
                 .resolve = [](vtbackend::ColorPalette const& p) { return (p.*Resolve)(Outcome); } };
    }

    /// The slot of the fold column's glyph colour in one state: the foreground of the palette's pair @p Pair,
    /// resolved by @p Resolve. Set, the pair keeps the background it had, or sits on the scheme's own -- the
    /// gutter draws no band of its own (@see the background row, which carries such a pair along).
    /// @param key   QML key.
    /// @param label Swatch label.
    /// @return The slot.
    template <auto Pair, auto Resolve>
    constexpr DerivedColorSlot foldMarkerSlot(std::string_view key, std::string_view label)
    {
        return { .key = key,
                 .label = label,
                 .own = [](vtbackend::ColorPalette const& p) -> std::optional<vtbackend::RGBColor> {
                     if (auto const& pair = p.*Pair)
                         return pair->foreground;
                     return std::nullopt;
                 },
                 .assign =
                     [](vtbackend::ColorPalette& p, std::optional<vtbackend::RGBColor> color) {
                         auto& pair = p.*Pair;
                         if (!color)
                         {
                             pair.reset();
                             return;
                         }
                         auto const background = pair ? pair->background : p.defaultBackground;
                         pair = vtbackend::RGBColorPair { .foreground = *color, .background = background };
                     },
                 .resolve = [](vtbackend::ColorPalette const& p) { return (p.*Resolve)().foreground; },
                 .sitsOn = [](vtbackend::ColorPalette const& p) { return (p.*Resolve)().background; } };
    }

    /// The slot of the block column's resting colour for @p Outcome (ColorPalette::blockStatusColor).
    template <vtbackend::CommandBlockOutcome Outcome>
    constexpr DerivedColorSlot blockStatusSlot(std::string_view key, std::string_view label)
    {
        return outcomeSlot<&vtbackend::ColorPalette::blockStatus,
                           &vtbackend::ColorPalette::blockStatusColor,
                           Outcome>(key, label);
    }

    /// The slot of the block column's colour for @p Outcome under the pointer
    /// (ColorPalette::blockStatusHoverColor).
    template <vtbackend::CommandBlockOutcome Outcome>
    constexpr DerivedColorSlot blockStatusHoverSlot(std::string_view key, std::string_view label)
    {
        return outcomeSlot<&vtbackend::ColorPalette::blockStatusHover,
                           &vtbackend::ColorPalette::blockStatusHoverColor,
                           Outcome>(key, label);
    }

    /// The slot of a colour the palette holds as a member of its own, @p Own, resolved by @p Resolve.
    /// @param key   QML key.
    /// @param label Swatch label.
    /// @return The slot.
    template <auto Own, auto Resolve, auto SitsOn = &pageBackground>
    constexpr DerivedColorSlot memberSlot(std::string_view key, std::string_view label)
    {
        return { .key = key,
                 .label = label,
                 .own = [](vtbackend::ColorPalette const& p) { return p.*Own; },
                 .assign = [](vtbackend::ColorPalette& p,
                              std::optional<vtbackend::RGBColor> color) { p.*Own = color; },
                 .resolve = [](vtbackend::ColorPalette const& p) { return (p.*Resolve)(); },
                 .sitsOn = SitsOn };
    }

    /// The command-block colour slots, in the order the editor lists them after the ANSI banks. Adding
    /// a derived slot to the scheme editor is adding a row here.
    constexpr auto DerivedColorSlots = std::array {
        foldMarkerSlot<&vtbackend::ColorPalette::foldMarker, &vtbackend::ColorPalette::foldMarkerColors>(
            "fold_marker", "Fold column"),
        foldMarkerSlot<&vtbackend::ColorPalette::foldMarkerHover,
                       &vtbackend::ColorPalette::foldMarkerHoverColors>("fold_marker_hover",
                                                                        "Fold column: hovered"),
        blockStatusSlot<vtbackend::CommandBlockOutcome::Success>("block_status_success",
                                                                 "Block status: success"),
        blockStatusSlot<vtbackend::CommandBlockOutcome::Failure>("block_status_failure",
                                                                 "Block status: failure"),
        blockStatusSlot<vtbackend::CommandBlockOutcome::Running>("block_status_running",
                                                                 "Block status: running"),
        blockStatusHoverSlot<vtbackend::CommandBlockOutcome::Success>("block_status_hover_success",
                                                                      "Block status hovered: success"),
        blockStatusHoverSlot<vtbackend::CommandBlockOutcome::Failure>("block_status_hover_failure",
                                                                      "Block status hovered: failure"),
        blockStatusHoverSlot<vtbackend::CommandBlockOutcome::Running>("block_status_hover_running",
                                                                      "Block status hovered: running"),
        memberSlot<&vtbackend::ColorPalette::gutterText, &vtbackend::ColorPalette::gutterTextColor>(
            "gutter_text", "Gutter text"),
        memberSlot<&vtbackend::ColorPalette::stickyHeaderBackground,
                   &vtbackend::ColorPalette::stickyHeaderBackgroundColor>("sticky_header_background",
                                                                          "Sticky header: background"),
        memberSlot<&vtbackend::ColorPalette::stickyHeaderSeparator,
                   &vtbackend::ColorPalette::stickyHeaderSeparatorColor,
                   [](vtbackend::ColorPalette const& p) {
                       return p.stickyHeaderBackgroundColor();
                   }>("sticky_header_separator", "Sticky header: separator"),
    };

    /// Where the colour a scheme editor row shows comes from.
    enum class SchemeColorSource : uint8_t
    {
        Scheme,  ///< A slot every scheme sets, such as the background.
        Derived, ///< A slot the scheme leaves unset, so the palette derives it from other colours.
        Pinned,  ///< A slot the palette could derive, set by the scheme to a colour of its own.
    };

    /// The QML-facing token of each SchemeColorSource, indexed by it.
    constexpr auto SchemeColorSourceTokens = std::array { "scheme"sv, "derived"sv, "pinned"sv };

    /// Which block of the Colors page's editor a slot sits in.
    enum class SchemeColorGroup : uint8_t
    {
        Base = 0,  ///< The default foreground and background.
        Normal,    ///< The eight normal ANSI colours.
        Bright,    ///< The eight bright ANSI colours.
        Interface, ///< The slots the palette derives for the gutter, folds, sticky header and block status.
    };

    /// The QML-facing token of each SchemeColorGroup, indexed by it.
    constexpr auto SchemeColorGroupTokens = std::array { "base"sv, "normal"sv, "bright"sv, "interface"sv };

    /// A data-driven descriptor for one editable color slot of a palette.
    struct SchemeColorDescriptor
    {
        QString key;
        QString label;
        std::function<vtbackend::RGBColor(vtbackend::ColorPalette const&)> get; //!< The colour in effect.
        std::function<void(vtbackend::ColorPalette&, vtbackend::RGBColor)>
            set; //!< Makes it the scheme's own.
        /// How the palette derives the slot while the scheme leaves it unset; none for one every scheme sets.
        DerivedColorSlot const* derived = nullptr;
        /// The colour the slot is drawn on; unset for the background, which sits on nothing the scheme has.
        std::function<vtbackend::RGBColor(vtbackend::ColorPalette const&)> sitsOn = &pageBackground;
        /// Which block of the editor the slot sits in.
        SchemeColorGroup group = SchemeColorGroup::Interface;

        /// Where the colour in effect comes from in @p palette.
        [[nodiscard]] SchemeColorSource source(vtbackend::ColorPalette const& palette) const
        {
            if (derived == nullptr)
                return SchemeColorSource::Scheme;
            return derived->own(palette) ? SchemeColorSource::Pinned : SchemeColorSource::Derived;
        }
    };

    /// The color slots the scheme editor exposes: the default fg/bg, the 8 normal and 8 bright ANSI
    /// colors (generated from the ANSI names so the two banks are not hand-listed), then the derived
    /// command-block slots of DerivedColorSlots.
    std::vector<SchemeColorDescriptor> const& schemeColorDescriptors()
    {
        static auto const descriptors = [] {
            auto list = std::vector<SchemeColorDescriptor> {};
            list.push_back({ .key = "background",
                             .label = "Background",
                             .get = [](auto const& p) { return p.defaultBackground; },
                             .set =
                                 [](vtbackend::ColorPalette& p, vtbackend::RGBColor c) {
                                     // A fold column the editor set sits on the page's background: it moves
                                     // with it, rather than leaving a band of the old one down the gutter.
                                     for (auto* const pair: { &p.foldMarker, &p.foldMarkerHover })
                                         if (*pair && (*pair)->background == p.defaultBackground)
                                             (*pair)->background = c;
                                     p.defaultBackground = c;
                                 },
                             .sitsOn = {},
                             .group = SchemeColorGroup::Base });
            list.push_back({ .key = "foreground",
                             .label = "Foreground",
                             .get = [](auto const& p) { return p.defaultForeground; },
                             .set = [](auto& p, auto c) { p.defaultForeground = c; },
                             .group = SchemeColorGroup::Base });
            static constexpr auto AnsiNames =
                std::array<std::string_view, 8> { "black", "red",     "green", "yellow",
                                                  "blue",  "magenta", "cyan",  "white" };
            for (auto const bright: { false, true })
                for (auto const i: std::views::iota(size_t { 0 }, AnsiNames.size()))
                {
                    auto const slot = bright ? i + 8 : i;
                    list.push_back({ .key = QString::fromStdString((bright ? "bright_" : "normal_")
                                                                   + std::string(AnsiNames[i])),
                                     .label = QString::fromStdString((bright ? "Bright " : "Normal ")
                                                                     + std::string(AnsiNames[i])),
                                     .get = [slot](auto const& p) { return p.palette.at(slot); },
                                     .set = [slot](auto& p, auto c) { p.palette.at(slot) = c; },
                                     .group = bright ? SchemeColorGroup::Bright : SchemeColorGroup::Normal });
                }
            for (auto const& derived: DerivedColorSlots)
                list.push_back({ .key = toQString(derived.key),
                                 .label = toQString(derived.label),
                                 .get = derived.resolve,
                                 .set = [assign = derived.assign](auto& p, auto c) { assign(p, c); },
                                 .derived = &derived,
                                 .sitsOn = derived.sitsOn });
            return list;
        }();
        return descriptors;
    }

    /// The scheme editor's slot named @p key, or nullptr when there is none.
    [[nodiscard]] SchemeColorDescriptor const* findSchemeColor(QString const& key)
    {
        auto const& descriptors = schemeColorDescriptors();
        auto const it = std::ranges::find(descriptors, key, &SchemeColorDescriptor::key);
        return it != descriptors.end() ? &*it : nullptr;
    }

    /// "#rrggbb" for @p color, the format the QML color pickers exchange.
    QString rgbToHex(vtbackend::RGBColor color)
    {
        return QString::asprintf("#%02x%02x%02x", color.red, color.green, color.blue);
    }

    /// The gallery card of scheme @p name drawn with @p palette: its background and foreground, and the eight
    /// normal colours as a strip.
    QVariantMap galleryCardOf(QString const& name, bool editable, vtbackend::ColorPalette const& palette)
    {
        static constexpr auto NormalColors = size_t { 8 };
        auto normal = QStringList {};
        for (auto const& color: palette.palette | std::views::take(NormalColors))
            normal.push_back(rgbToHex(color));
        auto card = QVariantMap {};
        card[QStringLiteral("name")] = name;
        card[QStringLiteral("editable")] = editable; // QML-facing bool
        card[QStringLiteral("background")] = rgbToHex(palette.defaultBackground);
        card[QStringLiteral("foreground")] = rgbToHex(palette.defaultForeground);
        card[QStringLiteral("normal")] = normal;
        return card;
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

    /// Where a row of the bindings viewer comes from.
    enum class BindingOrigin : uint8_t
    {
        Configured, ///< The configuration's `input_mapping:`.
        BuiltIn,    ///< A built-in fallback the configuration leaves free.
    };

    /// How the bindings viewer names a key.
    QString inputLabel(vtbackend::Key key)
    {
        return QString::fromStdString(std::format("{}", key));
    }

    /// How the bindings viewer names a mouse button.
    QString inputLabel(vtbackend::MouseButton button)
    {
        return QString::fromStdString(std::format("{}", button));
    }

    /// How the bindings viewer names a character: the whole codepoint.
    QString inputLabel(char32_t codepoint)
    {
        return QString::fromUcs4(&codepoint, 1);
    }

    /// Builds a read-only display row { trigger, action, mode, builtIn } for one input binding, or an empty
    /// map when the binding carries no action. `action` names every action the binding runs, in order.
    template <typename Input>
    QVariantMap keybindingRow(vtbackend::InputBinding<Input, config::ActionList> const& binding,
                              BindingOrigin origin)
    {
        if (binding.binding.empty())
            return {};
        auto const mods = formatModifiers(binding.modifiers);
        auto row = QVariantMap {};
        // The count is part of the trigger: Ctrl+Left x3 is not Ctrl+Left.
        auto const input = inputLabel(binding.input)
                           + QString::fromStdString(vtbackend::clickCountSuffix(binding.clickCount));
        row[QStringLiteral("trigger")] = mods.isEmpty() ? input : mods + '+' + input;
        auto actions = QStringList {};
        for (auto const& action: binding.binding)
            actions.push_back(QString::fromStdString(std::format("{}", action)));
        row[QStringLiteral("action")] = actions.join(QStringLiteral(", "));
        row[QStringLiteral("mode")] =
            binding.modes.any() ? QString::fromStdString(std::format("{}", binding.modes)) : QString {};
        row[QStringLiteral("builtIn")] = origin == BindingOrigin::BuiltIn;
        return row;
    }

    /// Every state the match modes can be in: each combination of the MatchModes flags, BlockFocus the
    /// highest.
    constexpr auto MatchModeStates =
        std::views::iota(0u, static_cast<unsigned>(vtbackend::MatchModes::BlockFocus) << 1);

    /// Whether a press can still reach @p fallback past @p configured, the configuration's bindings, which
    /// are looked up first: whether in some state of the match modes the fallback answers and no configured
    /// binding of the same chord and click count does. Decided on the states, not row by row, so two rows
    /// that cover the fallback's modes only together -- one on the alternate screen, one off it -- shadow it.
    template <typename Input>
    [[nodiscard]] bool reachable(
        vtbackend::InputBinding<Input, config::ActionList> const& fallback,
        std::vector<vtbackend::InputBinding<Input, config::ActionList>> const& configured)
    {
        return std::ranges::any_of(MatchModeStates, [&](unsigned state) {
            auto const flags = static_cast<uint8_t>(state);
            return config::helper::testMatchMode(flags, fallback.modes)
                   && std::ranges::none_of(configured, [&](auto const& binding) {
                          return binding.modifiers == fallback.modifiers && binding.input == fallback.input
                                 && binding.clickCount == fallback.clickCount
                                 && config::helper::testMatchMode(flags, binding.modes);
                      });
        });
    }

    /// Appends the bindings viewer's row for each of @p bindings, the configuration's own.
    template <typename Input>
    void appendConfiguredBindings(
        QVariantList& rows, std::vector<vtbackend::InputBinding<Input, config::ActionList>> const& bindings)
    {
        for (auto const& binding: bindings)
            if (auto row = keybindingRow(binding, BindingOrigin::Configured); !row.isEmpty())
                rows.push_back(std::move(row));
    }

    /// Appends the bindings viewer's row for each built-in fallback of @p fallbacks that @p config switches
    /// on and a press can reach past its @p configured bindings.
    template <typename Input>
    void appendBuiltInBindings(
        QVariantList& rows,
        std::vector<config::FallbackMapping<Input>> const& fallbacks,
        std::vector<vtbackend::InputBinding<Input, config::ActionList>> const& configured,
        config::Config const& config)
    {
        for (auto const& fallback: fallbacks)
            if (fallback.enabled(config) && reachable(fallback.mapping, configured))
                if (auto row = keybindingRow(fallback.mapping, BindingOrigin::BuiltIn); !row.isEmpty())
                    rows.push_back(std::move(row));
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

namespace
{
    using config::SettingsError;
    using config::SettingsErrorKind;

    /// A refusal of kind @p kind saying @p message.
    [[nodiscard]] std::unexpected<SettingsError> refuse(SettingsErrorKind kind, std::string message)
    {
        return std::unexpected(SettingsError { .kind = kind, .message = std::move(message) });
    }

    /// Whether @p config lets Settings write: not under `gui_config_locked`.
    [[nodiscard]] config::SettingsEditability editabilityOf(config::Config const& config)
    {
        return config.guiConfigLocked.value() ? config::SettingsEditability::Locked
                                              : config::SettingsEditability::Editable;
    }

    /// A name the user typed for a profile or a colour scheme, checked as a side file's name: never a path.
    [[nodiscard]] std::expected<void, SettingsError> checkNewName(std::string const& name)
    {
        return config::checkNewSideFileName(name).transform_error([](std::string reason) {
            return SettingsError { .kind = SettingsErrorKind::InvalidValue, .message = std::move(reason) };
        });
    }

    /// @return What a row's `actions` map says @p rule offers: { <ProfileActionTokens>: bool }.
    [[nodiscard]] QVariantMap actionsOffered(ProfileActionRule const& rule)
    {
        auto actions = QVariantMap {};
        for (auto const index: std::views::iota(std::size_t { 0 }, ProfileActionCount))
            actions[toQString(ProfileActionTokens.at(index))] =
                offers(rule, static_cast<ProfileAction>(index));
        return actions;
    }

    /// What every change says while `gui_config_locked` is set.
    [[nodiscard]] std::unexpected<SettingsError> lockedRefusal()
    {
        return refuse(SettingsErrorKind::Locked, std::string { config::SettingsLockedMessage });
    }

    /// Puts on field row @p row what the page needs to edit it: its `type` token, its `control`, an enum's
    /// `optionLabels`, and a number's `min`, `max`, `step`, `decimals` and `unit` (unbounded where the
    /// descriptor names no range).
    void describeControl(QVariantMap& row,
                         SettingsValueType type,
                         QStringList const& optionLabels,
                         std::optional<NumberRange> range,
                         std::optional<SettingsControl> control)
    {
        row[QStringLiteral("type")] = toQString(settingsValueTypeToken(type));
        auto const chosen = controlOf(type, optionLabels, range, control);
        row[QStringLiteral("control")] = static_cast<int>(chosen);
        row[QStringLiteral("controlName")] = toQString(settingsControlToken(chosen));
        if (type == SettingsValueType::Enum)
            row[QStringLiteral("optionLabels")] = optionLabels;
        if (type == SettingsValueType::Int || type == SettingsValueType::Double)
        {
            auto const shown = range.value_or(unboundedRange(type));
            row[QStringLiteral("min")] = shown.min;
            row[QStringLiteral("max")] = shown.max;
            row[QStringLiteral("step")] = shown.step;
            row[QStringLiteral("decimals")] = shown.decimals;
            row[QStringLiteral("unit")] = static_cast<int>(shown.unit);
            row[QStringLiteral("unitText")] = toQString(numberUnitRow(shown.unit).suffix);
            row[QStringLiteral("displayFactor")] = numberUnitRow(shown.unit).displayFactor;
        }
    }

    /// Forwards to the scheduler the host injected, and calls @p settled after each fire it runs: a
    /// debounced flush changes what "Revert Changes" can do, and nothing else tells the page.
    class NotifyingScheduler final: public config::SaveScheduler
    {
      public:
        NotifyingScheduler(std::shared_ptr<config::SaveScheduler> inner, std::function<void()> settled):
            _inner { std::move(inner) }, _settled { std::move(settled) }
        {
        }

        void schedule(std::chrono::milliseconds delay, std::function<void()> fire) override
        {
            _inner->schedule(delay, [fire = std::move(fire), settled = _settled] {
                fire();
                settled();
            });
        }

        void cancel() override { _inner->cancel(); }

      private:
        std::shared_ptr<config::SaveScheduler> _inner;
        std::function<void()> _settled;
    };
} // namespace

SettingsController::SettingsController(ConfigAccessor config,
                                       SettingsControllerServices services,
                                       vtbackend::ColorPreference appearance,
                                       QObject* parent):
    QObject { parent },
    _config { std::move(config) },
    _services { std::move(services) },
    _appearance { appearance },
    _scheduler { std::make_unique<NotifyingScheduler>(_services.scheduler,
                                                      [this] { emit changesChanged(); }) },
    _editability { editabilityOf(_config()) },
    _globalDraft { _config().guiManagedSettings }
{
    // The gallery shows the draft being edited over the schemes the store has.
    connect(this, &SettingsController::schemeDraftChanged, this, &SettingsController::schemeGalleryChanged);
    _session = makeSession();
    _session->observe(config::guiSettingsFile(), loadedBytesOf(config::guiSettingsFile()));
    refresh();
}

SettingsController::~SettingsController() = default;

std::unique_ptr<config::SettingsEditSession> SettingsController::makeSession()
{
    return std::make_unique<config::SettingsEditSession>(
        *_services.store, *_scheduler, _editability, _services.reload, [this](SettingsError const& error) {
            onSessionError(error);
        });
}

void SettingsController::setColorPreference(vtbackend::ColorPreference appearance)
{
    if (_appearance == appearance)
        return;
    _appearance = appearance;
    // The previews are derived from the draft on every draftChanged; nothing else depends on the mode.
    emit draftChanged();
}

vtbackend::ColorPalette SettingsController::readSchemePalette(std::string const& name) const
{
    auto const& cfg = _config();
    auto palette = vtbackend::ColorPalette {};
    if (auto const it = cfg.colorschemes.value().find(name); it != cfg.colorschemes.value().end())
        palette = it->second;
    else if (auto const loaded = sideFileScheme(name))
        palette = *loaded;
    else
        (void) vtbackend::defaultColorPalettes(name, palette);
    return palette;
}

vtbackend::ColorPalette SettingsController::schemePalette(std::string const& name) const
{
    if (_previewPaletteCache && _previewPaletteCache->name == name)
        return _previewPaletteCache->palette;
    auto palette = readSchemePalette(name);
    _previewPaletteCache = CachedPalette { .name = name, .palette = palette };
    return palette;
}

vtbackend::ColorPalette SettingsController::previewPalette() const
{
    if (_profileAccess == DraftAccess::Closed)
        return {};
    return schemePalette(previewSchemeNameOf(_draft));
}

std::string SettingsController::previewSchemeNameOf(config::TerminalProfile const& profile) const
{
    auto const& colors = profile.colors.value();
    if (auto const* const simple = std::get_if<config::SimpleColorConfig>(&colors))
        return simple->colorScheme;
    auto const& dual = std::get<config::DualColorConfig>(colors);
    switch (_appearance)
    {
        case vtbackend::ColorPreference::Light: return dual.colorSchemeLight;
        case vtbackend::ColorPreference::Dark: return dual.colorSchemeDark;
    }
    return dual.colorSchemeDark;
}

config::Config SettingsController::previewConfig() const
{
    auto preview = _config();
    if (_profileAccess != DraftAccess::Closed)
        preview.profiles.value().insert_or_assign(_editingProfile.toStdString(), _draft);
    if (_schemeAccess != DraftAccess::Closed)
        preview.colorschemes.value().insert_or_assign(_editingScheme.toStdString(), _schemeDraft);
    return preview;
}

std::string SettingsController::previewProfileName() const
{
    return _profileAccess != DraftAccess::Closed ? _editingProfile.toStdString()
                                                 : _config().defaultProfileName.value();
}

SettingsOrigin SettingsController::profileOrigin(std::string const& name) const
{
    auto const& origins = _config().profileOrigins;
    auto const it = origins.find(name);
    return it != origins.end() ? it->second : SettingsOrigin::Builtin;
}

bool SettingsController::colorSchemeSideFileExists(std::string const& name) const
{
    auto const file = config::colorSchemeFile(name);
    switch (_session->pendingFor(file))
    {
        case config::PendingChange::Write: return true;
        case config::PendingChange::Removal: return false;
        case config::PendingChange::None: break;
    }
    auto const read = _services.store->read(file);
    return read && read->has_value();
}

std::optional<std::string> SettingsController::loadedBytesOf(config::SideFile const& file) const
{
    auto const& loaded = _config().loadedSideFiles;
    auto const found = loaded.find(config::displayPathOf(file));
    return found != loaded.end() ? found->second : std::nullopt;
}

void SettingsController::followReloadedDrafts()
{
    if (_profileAccess == DraftAccess::Editable)
    {
        auto const file = config::profileFile(_editingProfile.toStdString());
        if (_session->pendingFor(file) == config::PendingChange::None
            && loadedBytesOf(file) != _draftSeededFrom
            && _config().findProfile(_editingProfile.toStdString()) != nullptr)
            editProfile(_editingProfile);
    }
    if (_hostAccess == DraftAccess::Editable && !openHostUnsaved())
    {
        auto const file = config::hostFile(_editingHost.toStdString());
        if (_session->pendingFor(file) == config::PendingChange::None
            && loadedBytesOf(file) != _hostSeededFrom
            && _config().hosts.value().contains(_editingHost.toStdString()))
            editHost(_editingHost);
    }
    if (_schemeAccess == DraftAccess::Editable)
    {
        auto const file = config::colorSchemeFile(_editingScheme.toStdString());
        auto const read = _services.store->read(file);
        if (read && read->has_value() && _session->pendingFor(file) == config::PendingChange::None
            && *read != _schemeSeededFrom)
            editColorScheme(_editingScheme);
    }
}

std::optional<vtbackend::ColorPalette> SettingsController::sideFileScheme(std::string const& name) const
{
    auto const file = config::colorSchemeFile(name);
    auto read = _services.store->read(file);
    if (!read || !read->has_value())
        return std::nullopt;
    return config::parseColorSchemeYaml(_config().configFile.parent_path() / config::relativePathOf(file),
                                        std::move(**read));
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

void SettingsController::fail(std::string const& error)
{
    emit errorOccurred(QString::fromStdString(error));
}

template <typename T>
void SettingsController::report(std::expected<T, config::SettingsError> const& result)
{
    // Every result reaches here from a flush or from a staging that a flush may have followed: a conflict
    // or a failed write re-seeds the drafts exactly as the debounced flush's error does.
    if (!result)
        onSessionError(result.error());
}

std::optional<std::string> SettingsController::patternProblemOf(TerminalProfile const& profile)
{
    // A field whose value has a problem (an invalid file_opener) is written as given: the reader keeps it and
    // reports it, and the terminal disables it, so a refusal would guard nothing. Its row shows the problem.
    //
    // A pattern the reader would skip, or one of the profile's own the terminal would disable: each row shows
    // its own. A built-in's broken override does not refuse the write: the page cannot change its regex, and
    // the reader keeps the entry and reports it at every load, so a refusal would guard nothing and block
    // every unrelated edit.
    for (auto const& pattern: profile.hintPatterns.value())
        if (auto const reason = config::skippedHintPatternReason(pattern))
            return std::format("Link patterns: {}.", *reason);
    for (auto const& diagnostic: config::linkPatternsFor(profile)->diagnostics())
        if (diagnostic.severity() == vtbackend::LinkPatternSeverity::Error
            && !vtbackend::isBuiltinLinkPatternName(diagnostic.pattern))
            return diagnostic.message();
    return std::nullopt;
}

std::optional<std::string> SettingsController::draftProblem() const
{
    return patternProblemOf(_draft);
}

// {{{ Session

bool SettingsController::hasChanges() const noexcept
{
    return _session->hasChanges();
}

void SettingsController::reseedAfter(SettingsError const& error)
{
    switch (error.kind)
    {
        case SettingsErrorKind::WriteFailed:
        case SettingsErrorKind::ChangedOnDisk: reseedDrafts(); break;
        case SettingsErrorKind::Locked:
        case SettingsErrorKind::ReadOnly:
        case SettingsErrorKind::InvalidValue:
        case SettingsErrorKind::UnknownSetting:
        case SettingsErrorKind::NotApplied:
        case SettingsErrorKind::NameTaken:
        case SettingsErrorKind::InUse:
        case SettingsErrorKind::ReloadFailed: break;
    }
}

void SettingsController::onSessionError(SettingsError const& error)
{
    // Re-seeded first: a draft that changes clears the page's error banner, which must then show this one.
    reseedAfter(error);
    fail(error.message);
}

std::expected<void, SettingsError> SettingsController::reseededOnFailure(
    std::expected<void, SettingsError> result)
{
    if (!result)
        reseedAfter(result.error());
    return result;
}

SettingsController::SchemeNames SettingsController::currentSchemeNames() const
{
    // Those the config knows about, unioned with any colorschemes/*.yml there is, then with what is staged:
    // a staged write is already there, and a staged removal is already gone.
    auto names = SchemeNames {};
    for (auto const& [name, _]: _config().colorschemes.value())
        names.all.insert(name);
    if (auto const listed = _services.store->list(config::SideFileKind::ColorScheme))
        for (auto const& file: *listed)
        {
            names.all.insert(file.name);
            names.editable.insert(file.name);
        }
    else
        errorLog()("Could not list the color schemes: {}", listed.error());
    for (auto const& pending: _session->pendingWrites(config::SideFileKind::ColorScheme))
    {
        names.all.insert(pending.name);
        names.editable.insert(pending.name);
    }
    for (auto const& removed: std::set<std::string>(names.all))
        if (_session->pendingFor(config::colorSchemeFile(removed)) == config::PendingChange::Removal)
        {
            names.all.erase(removed);
            names.editable.erase(removed);
        }
    return names;
}

void SettingsController::refreshSchemeList()
{
    rebuildSchemeList(currentSchemeNames());
    emit changed();
}

void SettingsController::rebuildSchemeList(SchemeNames const& names)
{
    _colorSchemes.clear();
    for (auto const& name: names.all)
    {
        auto row = QVariantMap {};
        row[QStringLiteral("name")] = QString::fromStdString(name);
        row[QStringLiteral("editable")] = (!locked() && names.editable.contains(name));
        _colorSchemes.push_back(row);
    }
}

void SettingsController::reseedGlobalDraft()
{
    _globalDraft = _config().guiManagedSettings;
    _session->observe(config::guiSettingsFile(), loadedBytesOf(config::guiSettingsFile()));
    emit globalFieldsChanged();
}

void SettingsController::reseedDrafts()
{
    reseedGlobalDraft();

    if (_hostAccess != DraftAccess::Closed)
    {
        if (_config().hosts.value().contains(_editingHost.toStdString()))
            editHost(_editingHost);
        else
            closeHostDraft();
        rebuildHostRows();
        emit changed();
    }

    if (_profileAccess != DraftAccess::Closed)
    {
        if (_config().findProfile(_editingProfile.toStdString()) != nullptr)
            editProfile(_editingProfile);
        else
        {
            _profileAccess = DraftAccess::Closed;
            _editingProfile.clear();
            emit draftChanged();
        }
    }

    if (_schemeAccess != DraftAccess::Closed)
    {
        if (_schemeAccess == DraftAccess::Editable
            && !colorSchemeSideFileExists(_editingScheme.toStdString()))
        {
            _schemeAccess = DraftAccess::Closed;
            _editingScheme.clear();
            emit schemeDraftChanged();
            emit editingSchemeChanged();
        }
        else
            editColorScheme(_editingScheme);
    }
}

void SettingsController::revertChanges()
{
    auto const reverted = _session->revert();
    reseedDrafts();
    if (!reverted)
        fail(reverted.error().message);
    emit changesChanged();
}

void SettingsController::flushPendingChanges()
{
    if (auto const flushed = _session->flush(); !flushed)
        onSessionError(flushed.error());
    emit changesChanged();
}

std::expected<void, SettingsError> SettingsController::stage(config::SideFile const& file,
                                                             std::optional<std::string> bytes)
{
    auto staged = _session->stage(file, std::move(bytes));
    emit changesChanged();
    return staged;
}

// }}}

void SettingsController::rebuildProfileRows()
{
    auto const& cfg = _config();
    _defaultProfile =
        QString::fromStdString(_globalDraft.defaultProfile.value_or(cfg.defaultProfileName.value()));
    // Name-sorted for a stable list.
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
        auto const& rule = profileActionRule(profileEditability(origin, _editability));
        row[QStringLiteral("editable")] = offers(rule, ProfileAction::Edit);
        row[QStringLiteral("banner")] = toQString(rule.banner);
        row[QStringLiteral("actions")] = actionsOffered(rule);
        row[QStringLiteral("isDefault")] = (QString::fromStdString(name) == _defaultProfile);
        _profiles.push_back(row);
    }
}

void SettingsController::refresh()
{
    _previewPaletteCache.reset(); // a scheme written, renamed or deleted, or the configuration reloaded
    auto const& cfg = _config();
    _gpus = _services.gpuInventory ? _services.gpuInventory->list() : std::vector<display::GpuCandidate> {};
    _fontFamilies.clear();
    for (auto const& family: (_services.fontFamilies ? _services.fontFamilies
                                                     : std::make_shared<platform::NullFontFamilySource>())
                                 ->monospaceFamilies())
        _fontFamilies.push_back(QString::fromStdString(family));
    _linkGesture =
        QString::fromStdString(command::describeLinkGesture(cfg, command::platformShortcutStyle()));
    listShellRows();

    // gui_config_locked configures the session rather than changing its state, so a change of it starts a
    // new one. Not here and now: refresh() runs inside the session's own reload. What the old session had
    // staged is dropped, not written -- once locked nothing may be, and a session that was locked staged
    // nothing -- and with it the snapshots Revert would have restored. The drafts are re-seeded, so that
    // none shows a value that is not on disk, and their editability follows the lock.
    if (editabilityOf(cfg) != _editability)
    {
        _editability = editabilityOf(cfg);
        QMetaObject::invokeMethod(
            this,
            [this] {
                _session = makeSession();
                reseedDrafts();
                emit changesChanged();
            },
            Qt::QueuedConnection);
    }

    if (_session->pendingFor(config::guiSettingsFile()) == config::PendingChange::None)
    {
        _globalDraft = cfg.guiManagedSettings;
        _session->observe(config::guiSettingsFile(), loadedBytesOf(config::guiSettingsFile()));
    }
    followReloadedDrafts();
    rebuildProfileRows();
    rebuildHostRows();

    auto const names = currentSchemeNames();
    auto const& schemeNames = names.all;
    auto const& onDisk = names.editable;

    rebuildSchemeList(names);
    _schemeGalleryCards.clear();
    _schemeListedBytes.clear();
    for (auto const& name: onDisk)
        if (auto const read = _services.store->read(config::colorSchemeFile(name)))
            _schemeListedBytes.emplace(name, *read);
    for (auto const& name: schemeNames)
    {
        _schemeGalleryCards.push_back(galleryCardOf(
            QString::fromStdString(name), !locked() && onDisk.contains(name), readSchemePalette(name)));
    }
    emit schemeGalleryChanged();

    // Keybindings (read-only): the configured key, character and mouse mappings, then the built-in ones a
    // press can still reach, so a binding Contour adds later shows up here without a configuration edit.
    _keybindings.clear();
    auto const& mappings = cfg.inputMappings.value();
    appendConfiguredBindings(_keybindings, mappings.keyMappings);
    appendConfiguredBindings(_keybindings, mappings.charMappings);
    appendConfiguredBindings(_keybindings, mappings.mouseMappings);
    appendBuiltInBindings(_keybindings, config::builtinFallbackKeyMappings(), mappings.keyMappings, cfg);
    appendBuiltInBindings(_keybindings, config::builtinFallbackCharMappings(), mappings.charMappings, cfg);
    appendBuiltInBindings(_keybindings, config::builtinFallbackMouseMappings(), mappings.mouseMappings, cfg);

    emit changed();
    emit globalFieldsChanged();
}

// {{{ Profile draft

QVariantList SettingsController::profileFields() const
{
    auto fields = QVariantList {};
    if (_profileAccess == DraftAccess::Closed)
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
            row[QStringLiteral("value")] = descriptor.get(_draft);
            row[QStringLiteral("problem")] =
                descriptor.validate ? descriptor.validate(row[QStringLiteral("value")]) : QString {};
            row[QStringLiteral("options")] = descriptor.options;
            describeControl(row,
                            descriptor.type,
                            descriptor.optionLabels.isEmpty() ? descriptor.options : descriptor.optionLabels,
                            descriptor.range,
                            descriptor.control);
            row[QStringLiteral("group")] = static_cast<int>(group.group);
            row[QStringLiteral("groupTitle")] = toQString(settingsGroupRow(group.group).title);
            row[QStringLiteral("groupGlyph")] = group.glyph;
            fields.push_back(row);
        }
    return fields;
}

std::expected<void, SettingsError> SettingsController::settlePendingOf(config::SideFile const& file)
{
    return _session->flushChangeOf(file);
}

std::expected<void, SettingsError> SettingsController::settleOpenProfile()
{
    if (_profileAccess != DraftAccess::Editable)
        return {};
    return settlePendingOf(config::profileFile(_editingProfile.toStdString()));
}

std::expected<void, SettingsError> SettingsController::settleOpenScheme()
{
    if (_schemeAccess != DraftAccess::Editable)
        return {};
    return settlePendingOf(config::colorSchemeFile(_editingScheme.toStdString()));
}

void SettingsController::editProfile(QString const& name)
{
    // The draft being left, and the profile being opened, may each hold an edit still inside the debounce:
    // it is written first, so that neither is read back from the configuration without it.
    report(settleOpenProfile());
    report(settlePendingOf(config::profileFile(name.toStdString())));
    auto const* profile = _config().findProfile(name.toStdString());
    if (profile == nullptr)
        return;
    _draft = *profile;
    _editingProfile = name;
    _profileAccess = draftAccessFor(profileOrigin(name.toStdString()));
    // What the draft was seeded from -- the bytes the configuration loaded, not the file as it is now -- is
    // what Settings last saw of it: an edit made outside it since that load is one it must not write over.
    _draftSeededFrom.reset();
    if (_profileAccess == DraftAccess::Editable)
    {
        auto const file = config::profileFile(name.toStdString());
        _draftSeededFrom = loadedBytesOf(file);
        _session->observe(file, _draftSeededFrom);
    }
    emit draftChanged();
}

std::expected<void, SettingsError> SettingsController::profileDraftWritable() const
{
    if (locked())
        return lockedRefusal();
    switch (_profileAccess)
    {
        case DraftAccess::Editable: return {};
        case DraftAccess::Closed: return refuse(SettingsErrorKind::ReadOnly, "No profile is open.");
        case DraftAccess::ReadOnly: break;
    }
    switch (profileOrigin(_editingProfile.toStdString()))
    {
        case SettingsOrigin::MainConfig:
            return refuse(SettingsErrorKind::ReadOnly, "Defined in contour.yml · Duplicate to Edit.");
        case SettingsOrigin::Builtin:
        case SettingsOrigin::SideFile: break;
    }
    return refuse(SettingsErrorKind::ReadOnly, "Built in · Duplicate to Edit.");
}

config::TerminalProfile SettingsController::writableForm(TerminalProfile const& profile)
{
    // A link pattern still in error is held back -- the reader would skip it, or the terminal disable it --
    // and its row says why; everything else in the draft is written meanwhile.
    auto writable = profile;
    if (patternProblemOf(profile))
    {
        auto const table = config::linkPatternsFor(profile);
        auto const inError = [&table](config::HintPatternConfig const& pattern) {
            if (config::skippedHintPatternReason(pattern))
                return true;
            return !vtbackend::isBuiltinLinkPatternName(pattern.name)
                   && std::ranges::any_of(table->diagnostics(), [&pattern](auto const& diagnostic) {
                          return diagnostic.pattern == pattern.name
                                 && diagnostic.severity() == vtbackend::LinkPatternSeverity::Error;
                      });
        };
        auto patterns = profile.hintPatterns.value();
        std::erase_if(patterns, inError);
        writable.hintPatterns = std::move(patterns);
    }
    return writable;
}

config::TerminalProfile SettingsController::writableDraft() const
{
    return writableForm(_draft);
}

std::expected<void, SettingsError> SettingsController::stageProfileDraft()
{
    // What it stages is what the reload that follows reads, so followReloadedDrafts() leaves the draft be.
    auto bytes = config::emitProfileYaml(writableDraft());
    auto staged = stage(config::profileFile(_editingProfile.toStdString()), bytes);
    if (staged)
        _draftSeededFrom = std::move(bytes);
    return staged;
}

void SettingsController::stageProfileDraftReporting()
{
    report(stageProfileDraft());
}

std::expected<void, SettingsError> SettingsController::setProfileField(QString const& key,
                                                                       QVariant const& value)
{
    if (auto const writable = profileDraftWritable(); !writable)
        return writable;
    for (auto const& group: profileFieldGroups())
        for (auto const& descriptor: group.fields)
            if (descriptor.key == key)
            {
                descriptor.set(_draft, value);
                emit draftChanged();
                return stageProfileDraft();
            }
    return refuse(SettingsErrorKind::UnknownSetting, "Unknown profile setting.");
}

void SettingsController::commitProfileField(QString const& key, QVariant const& value)
{
    report(setProfileField(key, value));
}

namespace
{
    /// Whether @p a and @p b are one name once case is folded: on Windows and macOS, `Work` and `work` are
    /// one file.
    [[nodiscard]] bool sameNameFolded(std::string const& a, std::string const& b)
    {
        return QString::fromStdString(a).toCaseFolded() == QString::fromStdString(b).toCaseFolded();
    }

    /// Whether @p newName may replace @p oldName as the name of a profile or a host: a side file's name, not
    /// @p oldName in another letter case, and not @p taken.
    /// @param what  What is renamed, for the refusal: "profile", "host".
    /// @param taken Whether a name is taken already.
    template <typename Taken>
    [[nodiscard]] std::expected<void, SettingsError> checkRename(std::string const& oldName,
                                                                 std::string const& newName,
                                                                 std::string_view what,
                                                                 Taken const& taken)
    {
        if (auto const valid = checkNewName(newName); !valid)
            return valid;
        // On a case-insensitive file system the new file would be the old one, which the rename then removes.
        if (sameNameFolded(newName, oldName))
            return refuse(
                SettingsErrorKind::NameTaken,
                "The new name differs from the current one only in letter case; choose another name.");
        if (taken(newName))
            return refuse(SettingsErrorKind::NameTaken,
                          std::format("A {} with that name already exists.", what));
        return {};
    }
} // namespace

bool SettingsController::sideFileNameTaken(config::SideFile const& file,
                                           std::vector<std::string> const& known) const
{
    auto const read = _services.store->read(file);
    auto const foldsEqual = [&file](std::string const& other) {
        return sameNameFolded(file.name, other);
    };
    return (read && read->has_value()) || _session->pendingFor(file) == config::PendingChange::Write
           || std::ranges::any_of(known, foldsEqual)
           || std::ranges::any_of(_session->pendingWrites(file.kind),
                                  [&](config::SideFile const& pending) { return foldsEqual(pending.name); });
}

bool SettingsController::isSideFile(config::SideFile const& file,
                                    std::optional<config::SettingsOrigin> saved) const
{
    switch (_session->pendingFor(file))
    {
        case config::PendingChange::Write: return true;
        case config::PendingChange::Removal: return false;
        case config::PendingChange::None: break;
    }
    return saved == SettingsOrigin::SideFile;
}

SettingsController::DraftAccess SettingsController::draftAccessFor(config::SettingsOrigin origin) const
{
    return offers(profileActionRule(profileEditability(origin, _editability)), ProfileAction::Edit)
               ? DraftAccess::Editable
               : DraftAccess::ReadOnly;
}

bool SettingsController::profileNameTaken(std::string const& name) const
{
    auto known = std::vector<std::string> {};
    for (auto const& [profile, _]: _config().profiles.value())
        known.push_back(profile);
    return sideFileNameTaken(config::profileFile(name), known);
}

bool SettingsController::isSideFileProfile(std::string const& name) const
{
    return isSideFile(config::profileFile(name), profileOrigin(name));
}

std::expected<QString, SettingsError> SettingsController::openNewProfile(std::string const& name,
                                                                         config::TerminalProfile profile)
{
    if (auto const settled = settleOpenProfile(); !settled)
        return std::unexpected(settled.error());
    if (auto const staged = stage(config::profileFile(name), config::emitProfileYaml(writableForm(profile)));
        !staged)
        return std::unexpected(staged.error());
    _draft = std::move(profile);
    _editingProfile = QString::fromStdString(name);
    _profileAccess = DraftAccess::Editable;
    emit draftChanged();
    return _editingProfile;
}

std::expected<QString, SettingsError> SettingsController::duplicateProfile(QString const& name)
{
    if (locked())
        return lockedRefusal();
    if (_editingProfile != name)
        if (auto const settled = settlePendingOf(config::profileFile(name.toStdString())); !settled)
            return std::unexpected(settled.error());
    // The open draft is what the page shows: a read-only one is the configuration's profile anyway.
    auto const* source = _profileAccess != DraftAccess::Closed && _editingProfile == name
                             ? &_draft
                             : _config().findProfile(name.toStdString());
    if (source == nullptr)
        return refuse(SettingsErrorKind::UnknownSetting,
                      std::format("There is no profile '{}'.", name.toStdString()));
    auto const copyName = uniqueCopyName(name.toStdString(), [this](std::string_view candidate) {
        return profileNameTaken(std::string { candidate });
    });
    if (auto const valid = checkNewName(copyName); !valid)
        return std::unexpected(valid.error());
    return openNewProfile(copyName, *source);
}

std::expected<QString, SettingsError> SettingsController::createProfile()
{
    if (locked())
        return lockedRefusal();
    auto const& cfg = _config();
    auto const* base = cfg.findProfile(cfg.defaultProfileName.value());
    // With no default profile to copy, the built-in defaults of the configuration's own edition.
    auto profile = base != nullptr ? *base : TerminalProfile { .defaultsEdition = cfg.defaultsEdition };
    auto const name = uniqueNewName(
        [this](std::string_view candidate) { return profileNameTaken(std::string { candidate }); });
    return openNewProfile(name, std::move(profile));
}

void SettingsController::duplicate(QString const& name)
{
    report(duplicateProfile(name));
}

void SettingsController::addProfile()
{
    report(createProfile());
}

// {{{ Host draft

namespace
{
    /// @return The row of a host's page that edits field @p key; null for a key no editable row has.
    [[nodiscard]] HostPageRowInfo const* editableHostRow(QString const& key)
    {
        auto const wanted = key.toStdString();
        auto const found = std::ranges::find_if(
            HostPageRows, [&wanted](HostPageRowInfo const& row) { return row.field && row.key == wanted; });
        return found != HostPageRows.end() ? &*found : nullptr;
    }
} // namespace

config::SettingsOrigin SettingsController::hostOrigin(std::string const& name) const
{
    auto const& origins = _config().hostOrigins;
    auto const it = origins.find(name);
    return it != origins.end() ? it->second : SettingsOrigin::SideFile;
}

std::optional<config::HostDraft> SettingsController::savedHostDraft(std::string const& name) const
{
    auto const& saved = _config().hosts.value();
    auto const found = saved.find(name);
    if (found == saved.end())
        return std::nullopt;
    if (hostOrigin(name) == SettingsOrigin::SideFile)
        if (auto written = loadedBytesOf(config::hostFile(name)).and_then(config::parseHostDraftYaml))
            return written;
    return config::hostDraftOf(found->second);
}

bool SettingsController::isSideFileHost(std::string const& name) const
{
    auto const& origins = _config().hostOrigins;
    auto const it = origins.find(name);
    return isSideFile(config::hostFile(name),
                      it != origins.end() ? std::optional { it->second } : std::nullopt);
}

bool SettingsController::openHostUnsaved() const
{
    auto const name = _editingHost.toStdString();
    return _hostAccess == DraftAccess::Editable && !isSideFileHost(name)
           && !_config().hostOrigins.contains(name);
}

bool SettingsController::hostNameTaken(std::string const& name) const
{
    auto known = std::vector<std::string> {};
    for (auto const& [host, _]: _config().hosts.value())
        known.push_back(host);
    // The open host, still to be saved, has the name too.
    if (_hostAccess != DraftAccess::Closed)
        known.push_back(_editingHost.toStdString());
    return sideFileNameTaken(config::hostFile(name), known);
}

void SettingsController::rebuildHostRows()
{
    auto const& saved = _config().hosts.value();
    auto names = std::set<std::string> {};
    for (auto const& [name, _]: saved)
        names.insert(name);
    for (auto const& pending: _session->pendingWrites(config::SideFileKind::Host))
        names.insert(pending.name);
    std::erase_if(names, [this](std::string const& name) {
        return _session->pendingFor(config::hostFile(name)) == config::PendingChange::Removal;
    });
    if (_hostAccess != DraftAccess::Closed)
        names.insert(_editingHost.toStdString());

    _hosts.clear();
    for (auto const& name: names)
    {
        auto const origin = hostOrigin(name);
        // The open draft is what the page shows; another is as the configuration has it.
        auto scheme = std::optional<config::HostScheme> {};
        if (_hostAccess != DraftAccess::Closed && _editingHost.toStdString() == name)
            scheme = hostDraftScheme();
        else if (auto const found = saved.find(name); found != saved.end())
            scheme = found->second.url.scheme;
        auto const& rule = profileActionRule(profileEditability(origin, _editability));
        auto row = QVariantMap {};
        row[QStringLiteral("name")] = QString::fromStdString(name);
        row[QStringLiteral("origin")] = originString(origin);
        row[QStringLiteral("editable")] = offers(rule, ProfileAction::Edit);
        row[QStringLiteral("transport")] = scheme ? toQString(transportLabel(*scheme)) : QString {};
        row[QStringLiteral("file")] = QString::fromStdString(config::displayPathOf(config::hostFile(name)));
        row[QStringLiteral("banner")] = toQString(rule.banner);
        auto actions = actionsOffered(rule);
        actions[QStringLiteral("attach")] = saved.contains(name);
        row[QStringLiteral("actions")] = actions;
        _hosts.push_back(row);
    }
}

std::optional<config::HostScheme> SettingsController::hostDraftScheme() const
{
    if (auto const scheme = config::hostSchemeOf(_hostDraft.url))
        return scheme;
    return _hostScheme;
}

QVariantList SettingsController::hostFields() const
{
    auto fields = QVariantList {};
    if (_hostAccess == DraftAccess::Closed)
        return fields;
    auto const problems = config::checkHostDraft(_hostDraft);
    auto const scheme = hostDraftScheme();
    for (auto const& info: HostPageRows)
    {
        if (!hostPageShows(info, scheme))
            continue;
        auto row = QVariantMap {};
        row[QStringLiteral("key")] = toQString(info.key);
        row[QStringLiteral("label")] = toQString(info.label);
        row[QStringLiteral("help")] = toQString(info.help);
        row[QStringLiteral("placeholder")] = toQString(info.placeholder);
        row[QStringLiteral("group")] = static_cast<int>(info.group);
        row[QStringLiteral("groupTitle")] = toQString(settingsGroupRow(info.group).title);
        row[QStringLiteral("canChoose")] =
            info.control == SettingsControl::PathField && static_cast<bool>(_services.chooseFile);
        auto problem = QString {};
        if (info.field)
        {
            row[QStringLiteral("value")] =
                QString::fromStdString(_hostDraft.*config::hostDraftMember(*info.field));
            if (auto const found = std::ranges::find(problems, *info.field, &config::HostFieldProblem::field);
                found != problems.end())
                problem = QString::fromStdString(found->message);
        }
        else
            row[QStringLiteral("value")] = scheme ? toQString(transportLabel(*scheme)) : QString {};
        row[QStringLiteral("problem")] = problem;

        auto options = QStringList {};
        auto labels = QStringList {};
        auto type = SettingsValueType::String;
        if (info.row == HostPageRow::Profile)
        {
            // Empty is the configuration's default profile.
            type = SettingsValueType::Enum;
            options.push_back(QString {});
            labels.push_back(QStringLiteral("Default"));
            for (auto const& profile: _profiles)
            {
                auto const name = profile.toMap().value(QStringLiteral("name")).toString();
                options.push_back(name);
                labels.push_back(name);
            }
            // A profile the configuration no longer has is still what the host names.
            if (auto const current = row.value(QStringLiteral("value")).toString();
                !options.contains(current))
            {
                options.push_back(current);
                labels.push_back(current);
            }
        }
        row[QStringLiteral("options")] = options;
        describeControl(row, type, labels, std::nullopt, info.control);
        fields.push_back(row);
    }
    return fields;
}

bool SettingsController::hostAttachable() const
{
    return _hostAccess != DraftAccess::Closed && config::checkHostDraft(_hostDraft).empty()
           && (_config().hosts.value().contains(_editingHost.toStdString())
               || _session->pendingFor(config::hostFile(_editingHost.toStdString()))
                      == config::PendingChange::Write);
}

void SettingsController::closeHostDraft()
{
    _hostAccess = DraftAccess::Closed;
    _editingHost.clear();
    _hostDraft = {};
    _hostScheme.reset();
    _hostSeededFrom.reset();
    emit hostDraftChanged();
}

void SettingsController::editHost(QString const& name)
{
    // The host being left, and the one being opened, may each hold an edit still inside the debounce.
    if (_hostAccess == DraftAccess::Editable && !openHostUnsaved())
        report(settlePendingOf(config::hostFile(_editingHost.toStdString())));
    report(settlePendingOf(config::hostFile(name.toStdString())));
    auto draft = savedHostDraft(name.toStdString());
    if (!draft)
        return;
    auto const wasUnsaved = openHostUnsaved() && _editingHost != name;
    auto const origin = hostOrigin(name.toStdString());
    _hostAccess = draftAccessFor(origin);
    _editingHost = name;
    _hostDraft = std::move(*draft);
    _hostSeededFrom.reset();
    if (origin == SettingsOrigin::SideFile)
    {
        auto const file = config::hostFile(name.toStdString());
        _hostSeededFrom = loadedBytesOf(file);
        if (_hostAccess == DraftAccess::Editable)
            _session->observe(file, _hostSeededFrom);
    }
    _hostScheme = config::hostSchemeOf(_hostDraft.url);
    if (wasUnsaved)
    {
        // The host left was never written: its row goes with it.
        rebuildHostRows();
        emit changed();
    }
    emit hostDraftChanged();
}

std::expected<void, SettingsError> SettingsController::hostDraftWritable() const
{
    if (locked())
        return lockedRefusal();
    switch (_hostAccess)
    {
        case DraftAccess::Editable: return {};
        case DraftAccess::Closed: return refuse(SettingsErrorKind::ReadOnly, "No host is open.");
        case DraftAccess::ReadOnly: break;
    }
    return refuse(SettingsErrorKind::ReadOnly, "Defined in contour.yml · Duplicate to Edit.");
}

std::expected<void, SettingsError> SettingsController::stageHostDraft()
{
    // A page with a problem saves nothing: its rows say what, and the file keeps what it last had.
    if (!config::checkHostDraft(_hostDraft).empty())
        return {};
    auto const wasUnsaved = openHostUnsaved();
    auto bytes = config::emitHostYaml(_hostDraft);
    auto staged = stage(config::hostFile(_editingHost.toStdString()), bytes);
    if (!staged)
        return staged;
    _hostSeededFrom = std::move(bytes);
    if (wasUnsaved)
    {
        rebuildHostRows();
        emit changed();
    }
    return staged;
}

std::expected<void, SettingsError> SettingsController::setHostField(QString const& key, QVariant const& value)
{
    if (auto const writable = hostDraftWritable(); !writable)
        return writable;
    auto const* const row = editableHostRow(key);
    if (row == nullptr)
        return refuse(SettingsErrorKind::UnknownSetting, "Unknown host setting.");
    _hostDraft.*config::hostDraftMember(*row->field) = value.toString().trimmed().toStdString();
    if (auto const scheme = config::hostSchemeOf(_hostDraft.url))
        _hostScheme = scheme;
    // Told after the staging: whether the host can be attached to follows what is staged for it.
    auto staged = stageHostDraft();
    emit hostDraftChanged();
    return staged;
}

void SettingsController::commitHostField(QString const& key, QVariant const& value)
{
    report(setHostField(key, value));
}

void SettingsController::chooseHostFile(QString const& key)
{
    if (!_services.chooseFile || !hostDraftWritable())
        return;
    auto const* const row = editableHostRow(key);
    if (row == nullptr)
        return;
    auto const& current = _hostDraft.*config::hostDraftMember(*row->field);
    if (auto const chosen =
            _services.chooseFile(std::string { row->label }, std::filesystem::path { current }))
        commitHostField(key, QString::fromStdString(core::pathToUtf8(*chosen)));
}

std::expected<QString, SettingsError> SettingsController::openNewHost(std::string const& name,
                                                                      config::HostDraft draft)
{
    if (auto const valid = checkNewName(name); !valid)
        return std::unexpected(valid.error());
    if (_hostAccess == DraftAccess::Editable && !openHostUnsaved())
        if (auto const settled = settlePendingOf(config::hostFile(_editingHost.toStdString())); !settled)
            return std::unexpected(settled.error());
    _hostDraft = std::move(draft);
    _editingHost = QString::fromStdString(name);
    _hostAccess = DraftAccess::Editable;
    _hostScheme = config::hostSchemeOf(_hostDraft.url);
    _hostSeededFrom.reset();
    if (auto const staged = stageHostDraft(); !staged)
    {
        closeHostDraft();
        return std::unexpected(staged.error());
    }
    rebuildHostRows();
    emit changed();
    emit hostDraftChanged();
    return _editingHost;
}

std::expected<QString, SettingsError> SettingsController::createHost()
{
    if (locked())
        return lockedRefusal();
    constexpr auto NewHostName = std::string_view { "New Host" };
    auto const name = firstFreeName(
        NewHostName, [this](std::string_view candidate) { return hostNameTaken(std::string { candidate }); });
    return openNewHost(name, config::HostDraft {});
}

std::expected<QString, SettingsError> SettingsController::duplicateHost(QString const& name)
{
    if (locked())
        return lockedRefusal();
    // The open draft is what the page shows; another host is as it is saved.
    auto source = _hostAccess != DraftAccess::Closed && _editingHost == name
                      ? std::optional { _hostDraft }
                      : savedHostDraft(name.toStdString());
    if (!source)
        return refuse(SettingsErrorKind::UnknownSetting,
                      std::format("There is no host '{}'.", name.toStdString()));
    auto const copyName = uniqueCopyName(name.toStdString(), [this](std::string_view candidate) {
        return hostNameTaken(std::string { candidate });
    });
    return openNewHost(copyName, std::move(*source));
}

std::expected<void, SettingsError> SettingsController::deleteHost(QString const& name)
{
    if (locked())
        return lockedRefusal();
    auto const isOpen = _hostAccess != DraftAccess::Closed && _editingHost == name;
    if (isOpen && openHostUnsaved())
    {
        // Never written: there is no file to remove.
        closeHostDraft();
        rebuildHostRows();
        emit changed();
        return {};
    }
    if (!isSideFileHost(name.toStdString()))
        return refuse(SettingsErrorKind::ReadOnly, "Only hosts made in Settings can be deleted here.");

    auto const file = config::hostFile(name.toStdString());
    if (!isOpen)
        _session->observe(file, loadedBytesOf(file));
    if (auto const staged = stage(file, std::nullopt); !staged)
        return staged;
    if (isOpen)
        closeHostDraft();
    rebuildHostRows();
    emit changed();
    return {};
}

std::expected<void, SettingsError> SettingsController::renameHost(QString const& oldName,
                                                                  QString const& newName)
{
    if (locked())
        return lockedRefusal();
    auto const isOpen = _hostAccess != DraftAccess::Closed && _editingHost == oldName;
    auto const unsaved = isOpen && openHostUnsaved();
    if (!unsaved && !isSideFileHost(oldName.toStdString()))
        return refuse(SettingsErrorKind::ReadOnly, "Only hosts made in Settings can be renamed here.");
    auto const trimmed = newName.trimmed();
    if (trimmed == oldName)
        return {};
    if (auto const valid =
            checkRename(oldName.toStdString(),
                        trimmed.toStdString(),
                        "host",
                        [this](std::string const& candidate) { return hostNameTaken(candidate); });
        !valid)
        return valid;

    if (!unsaved)
    {
        if (!isOpen)
            if (auto const settled = settlePendingOf(config::hostFile(oldName.toStdString())); !settled)
                return settled;
        auto const draft = isOpen ? std::optional { _hostDraft } : savedHostDraft(oldName.toStdString());
        if (!draft || !config::checkHostDraft(*draft).empty())
            return refuse(SettingsErrorKind::InvalidValue, "Correct the host's settings before renaming it.");
        if (!isOpen)
            _session->observe(config::hostFile(oldName.toStdString()),
                              loadedBytesOf(config::hostFile(oldName.toStdString())));
        // One change: the new file and the old one's removal are written together or not at all.
        auto const files = std::vector<config::StagedFile> {
            { .file = config::hostFile(trimmed.toStdString()), .bytes = config::emitHostYaml(*draft) },
            { .file = config::hostFile(oldName.toStdString()), .bytes = std::nullopt },
        };
        auto staged = _session->stageTogether(files);
        emit changesChanged();
        if (!staged)
            return staged;
    }

    if (isOpen)
        _editingHost = trimmed;
    emit hostRenamed(oldName, trimmed);
    rebuildHostRows();
    emit changed();
    if (isOpen)
        emit hostDraftChanged();
    return {};
}

std::expected<void, SettingsError> SettingsController::attachToHost(QString const& name)
{
    if (_hostAccess != DraftAccess::Closed && _editingHost == name
        && !config::checkHostDraft(_hostDraft).empty())
        return refuse(SettingsErrorKind::InvalidValue, "Correct the host's settings before attaching to it.");
    // The window it opens reads the host from the configuration: what is staged for it is written first.
    if (auto const settled = settlePendingOf(config::hostFile(name.toStdString())); !settled)
        return settled;
    if (!_config().hosts.value().contains(name.toStdString()))
        return refuse(SettingsErrorKind::UnknownSetting,
                      std::format("There is no saved host '{}'.", name.toStdString()));
    if (_services.runAction)
        _services.runAction(actions::AttachToHost { .name = name.toStdString() });
    return {};
}

void SettingsController::addHost()
{
    report(createHost());
}

void SettingsController::requestDuplicateHost(QString const& name)
{
    report(duplicateHost(name));
}

void SettingsController::requestDeleteHost(QString const& name)
{
    report(deleteHost(name));
}

void SettingsController::requestRenameHost(QString const& oldName, QString const& newName)
{
    report(renameHost(oldName, newName));
}

void SettingsController::requestAttachHost(QString const& name)
{
    report(attachToHost(name));
}

// }}}

QString SettingsController::colorSchemeMode() const
{
    if (_profileAccess != DraftAccess::Closed
        && std::holds_alternative<config::DualColorConfig>(_draft.colors.value()))
        return QStringLiteral("dual");
    return QStringLiteral("simple");
}

QString SettingsController::colorScheme() const
{
    if (_profileAccess != DraftAccess::Closed)
        if (auto const* simple = std::get_if<config::SimpleColorConfig>(&_draft.colors.value()))
            return QString::fromStdString(simple->colorScheme);
    return {};
}

QString SettingsController::colorSchemeLight() const
{
    if (_profileAccess != DraftAccess::Closed)
        if (auto const* dual = std::get_if<config::DualColorConfig>(&_draft.colors.value()))
            return QString::fromStdString(dual->colorSchemeLight);
    return {};
}

QString SettingsController::colorSchemeDark() const
{
    if (_profileAccess != DraftAccess::Closed)
        if (auto const* dual = std::get_if<config::DualColorConfig>(&_draft.colors.value()))
            return QString::fromStdString(dual->colorSchemeDark);
    return {};
}

void SettingsController::setColorSchemeMode(QString const& mode)
{
    if (auto const writable = profileDraftWritable(); !writable)
    {
        report(writable);
        return;
    }
    auto& colors = _draft.colors.value();
    if (mode == QStringLiteral("dual") && std::holds_alternative<config::SimpleColorConfig>(colors))
    {
        auto const name = std::get<config::SimpleColorConfig>(colors).colorScheme;
        colors = config::DualColorConfig { .colorSchemeLight = name, .colorSchemeDark = name };
    }
    else if (mode == QStringLiteral("simple") && std::holds_alternative<config::DualColorConfig>(colors))
    {
        auto const name = std::get<config::DualColorConfig>(colors).colorSchemeDark;
        colors = config::SimpleColorConfig { .colorScheme = name };
    }
    else
        return;
    emit draftChanged();
    stageProfileDraftReporting();
}

void SettingsController::setColorScheme(QString const& name)
{
    if (auto const writable = profileDraftWritable(); !writable)
    {
        report(writable);
        return;
    }
    if (auto* simple = std::get_if<config::SimpleColorConfig>(&_draft.colors.value()))
    {
        simple->colorScheme = name.toStdString();
        emit draftChanged();
        stageProfileDraftReporting();
    }
}

void SettingsController::setColorSchemeLight(QString const& name)
{
    if (auto const writable = profileDraftWritable(); !writable)
    {
        report(writable);
        return;
    }
    if (auto* dual = std::get_if<config::DualColorConfig>(&_draft.colors.value()))
    {
        dual->colorSchemeLight = name.toStdString();
        emit draftChanged();
        stageProfileDraftReporting();
    }
}

void SettingsController::setColorSchemeDark(QString const& name)
{
    if (auto const writable = profileDraftWritable(); !writable)
    {
        report(writable);
        return;
    }
    if (auto* dual = std::get_if<config::DualColorConfig>(&_draft.colors.value()))
    {
        dual->colorSchemeDark = name.toStdString();
        emit draftChanged();
        stageProfileDraftReporting();
    }
}

std::expected<void, SettingsError> SettingsController::deleteProfile(QString const& name)
{
    if (locked())
        return lockedRefusal();
    if (!isSideFileProfile(name.toStdString()))
        return refuse(SettingsErrorKind::ReadOnly, "Only profiles made in Settings can be deleted here.");

    // The open draft's file was observed when it was opened. Another is as the loader read it, as in a
    // rename: a hand edit made since is a conflict, not removed unseen.
    if (_editingProfile != name)
        _session->observe(config::profileFile(name.toStdString()),
                          loadedBytesOf(config::profileFile(name.toStdString())));
    if (auto const staged = stage(config::profileFile(name.toStdString()), std::nullopt); !staged)
        return staged;

    if (_editingProfile == name)
    {
        _profileAccess = DraftAccess::Closed;
        _editingProfile.clear();
        emit draftChanged();
    }
    return {};
}

void SettingsController::requestDeleteProfile(QString const& name)
{
    report(deleteProfile(name));
}

// }}}

std::expected<void, SettingsError> SettingsController::renameProfile(QString const& oldName,
                                                                     QString const& newName)
{
    if (locked())
        return lockedRefusal();
    if (!isSideFileProfile(oldName.toStdString()))
        return refuse(SettingsErrorKind::ReadOnly, "Only profiles made in Settings can be renamed here.");
    auto const trimmed = newName.trimmed();
    if (trimmed == oldName)
        return {}; // nothing to do
    if (auto const valid =
            checkRename(oldName.toStdString(),
                        trimmed.toStdString(),
                        "profile",
                        [this](std::string const& candidate) { return profileNameTaken(candidate); });
        !valid)
        return valid;

    // The open draft carries what is staged for the profile; another profile is as the configuration has it,
    // so what is still inside the debounce for it is written first.
    if (_editingProfile != oldName)
        if (auto const settled = settlePendingOf(config::profileFile(oldName.toStdString())); !settled)
            return settled;
    // What is written is the draft as the page writes it: a link pattern in error is held back.
    auto const openCopy = _editingProfile == oldName ? std::optional { writableDraft() } : std::nullopt;
    auto const* profile = openCopy ? &*openCopy : _config().findProfile(oldName.toStdString());
    if (profile == nullptr)
        return refuse(SettingsErrorKind::UnknownSetting, "Open the profile to rename it.");

    // The bytes below are the configuration's, so what the session saw of the old file is what the loader
    // read: a hand edit made since is then a conflict, not lost with the removal.
    if (_editingProfile != oldName)
        _session->observe(config::profileFile(oldName.toStdString()),
                          loadedBytesOf(config::profileFile(oldName.toStdString())));

    // One change: the new file, the old one's removal, and the default that follows it are written together
    // or not at all.
    auto files = std::vector<config::StagedFile> {
        { .file = config::profileFile(trimmed.toStdString()), .bytes = config::emitProfileYaml(*profile) },
        { .file = config::profileFile(oldName.toStdString()), .bytes = std::nullopt },
    };
    auto defaultDraft = _globalDraft;
    auto const followsDefault = _defaultProfile == oldName;
    if (followsDefault)
    {
        if (auto const writable = globalDraftWritable(); !writable)
            return writable;
        defaultDraft.defaultProfile = trimmed.toStdString();
        files.push_back(
            { .file = config::guiSettingsFile(), .bytes = config::emitGuiSettingsYaml(defaultDraft) });
    }
    auto staged = _session->stageTogether(files);
    emit changesChanged();
    if (!staged)
        return staged;

    // The profile the page edits is the renamed one before anything is announced, so that a page of the old
    // name is moved to the new one rather than dropped for a profile that is gone.
    auto const wasEditing = _editingProfile == oldName;
    if (wasEditing)
        _editingProfile = trimmed;
    emit profileRenamed(oldName, trimmed);
    if (followsDefault)
    {
        _globalDraft = std::move(defaultDraft);
        rebuildProfileRows();
        emit changed();
        emit globalFieldsChanged();
    }
    if (wasEditing)
        emit draftChanged();
    return {};
}

void SettingsController::requestRenameProfile(QString const& oldName, QString const& newName)
{
    report(renameProfile(oldName, newName));
}

std::expected<void, SettingsError> SettingsController::globalDraftWritable() const
{
    if (locked())
        return lockedRefusal();
    // Rewriting a settings.yml the loader could not read would replace everything in it with the few keys
    // the page knows.
    if (_config().guiSettingsFileHealth == config::GuiSettingsFileHealth::Unparsable)
        return refuse(SettingsErrorKind::ReadOnly, "settings.yml does not parse; fix it in an editor first.");
    return {};
}

std::expected<void, SettingsError> SettingsController::stageGlobalDraft()
{
    auto staged = stage(config::guiSettingsFile(), config::emitGuiSettingsYaml(_globalDraft));
    if (!staged)
        reseedGlobalDraft();
    else
        emit globalFieldsChanged();
    return staged;
}

std::expected<void, SettingsError> SettingsController::setDefaultProfile(QString const& name)
{
    if (auto const writable = globalDraftWritable(); !writable)
        return writable;
    _globalDraft.defaultProfile = name.toStdString();
    auto staged = stageGlobalDraft();
    // The list's default mark follows the draft; nothing else in the lists changed.
    rebuildProfileRows();
    emit changed();
    return staged;
}

void SettingsController::requestDefaultProfile(QString const& name)
{
    report(setDefaultProfile(name));
}

QVariantList SettingsController::globalFields() const
{
    auto const& cfg = _config();
    auto const& inEffect = cfg.guiManagedSettings.globalOverrides;
    auto const& committed = _globalDraft.globalOverrides;
    auto fields = QVariantList {};
    for (auto const& descriptor: globalFieldDescriptors())
    {
        auto row = QVariantMap {};
        auto const key = descriptor.key.toStdString();
        row[QStringLiteral("key")] = descriptor.key;
        row[QStringLiteral("label")] = descriptor.label;
        row[QStringLiteral("help")] = descriptor.help;
        row[QStringLiteral("group")] = static_cast<int>(descriptor.group);
        row[QStringLiteral("groupTitle")] = toQString(settingsGroupRow(descriptor.group).title);
        row[QStringLiteral("value")] = descriptor.get(cfg);
        // The operating system's refusal of the hotkey in effect, or of the chord just recorded.
        if (descriptor.key == HotkeyKey)
            row[QStringLiteral("problem")] = _hotkeyProblem;
        if (descriptor.optionsProvider)
        {
            auto values = QStringList {};
            auto labels = QStringList {};
            for (auto const& option:
                 descriptor.optionsProvider(OptionContext { .config = cfg, .gpus = _gpus }))
            {
                values.push_back(option.value);
                labels.push_back(option.label);
            }
            row[QStringLiteral("options")] = values;
            describeControl(row, descriptor.type, labels, descriptor.range, descriptor.control);
        }
        else
        {
            row[QStringLiteral("options")] = descriptor.options;
            describeControl(row,
                            descriptor.type,
                            descriptor.optionLabels.isEmpty() ? descriptor.options : descriptor.optionLabels,
                            descriptor.range,
                            descriptor.control);
        }
        auto const stored = committed.find(key);
        auto const effective = inEffect.find(key);
        if (stored != committed.end() && (effective == inEffect.end() || effective->second != stored->second))
        {
            // Committed and not yet written: the row shows what the user chose, which was validated as the
            // loader reads it before it was staged.
            row[QStringLiteral("value")] = descriptor.fromYaml(stored->second);
            row[QStringLiteral("overridden")] = true;
        }
        else
            // Overridden is what took: an override the loader refused -- a bool spelled "maybe" -- leaves the
            // value contour.yml gives, and a Reset beside it would offer to undo a change that never
            // happened. One that took is the value in effect once read back as the loader reads it.
            row[QStringLiteral("overridden")] =
                stored != committed.end()
                && descriptor.fromYaml(stored->second) == row[QStringLiteral("value")];
        fields.push_back(row);
    }
    return fields;
}

namespace
{
    /// The QML-facing token of a scope.
    [[nodiscard]] QString scopeToken(SettingsScope scope)
    {
        switch (scope)
        {
            case SettingsScope::Global: return QStringLiteral("global");
            case SettingsScope::Profile: return QStringLiteral("profile");
            case SettingsScope::Host: return QStringLiteral("host");
        }
        return QStringLiteral("global");
    }
} // namespace

std::expected<platform::RcInspection, platform::ShellRcError> SettingsController::inspectShell(
    config::ShellKind shell) const
{
    return _services.shell.editor->inspect(shell, rcEnvironment());
}

platform::RcEnvironment SettingsController::rcEnvironment() const
{
    auto const* const profile = _config().findProfile(_config().defaultProfileName.value());
    if (profile == nullptr)
        return {};
    auto const& shell = profile->shell.value();
    return platform::rcEnvironmentOf(shell.env, shell.removedEnvironment);
}

InstallBlock SettingsController::installBlock() const
{
    auto const reach =
        _services.shell.contourBinary ? _services.shell.contourBinary().reach : BinaryReach::Unreachable;
    return installBlockOf(_services.shell.host, reach);
}

void SettingsController::listShellRows()
{
    _shellRows.clear();
    if (_services.shell.editor != nullptr)
    {
        auto const block = installBlock();
        for (auto const& shell: config::ShellRows)
        {
            auto const row =
                shellInstallRowOf(shell, inspectShell(shell.kind), _services.shell.homeDirectory, block);
            _shellRows.push_back(QVariantMap {
                { QStringLiteral("shell"), static_cast<int>(row.shell) },
                { QStringLiteral("name"), toQString(row.name) },
                { QStringLiteral("status"), toQString(row.status) },
                { QStringLiteral("deleteLine"), toQString(row.deleteLine) },
                { QStringLiteral("canInstall"), row.install == InstallOffer::Yes },
            });
        }
    }
    emit shellIntegrationRowsChanged();
}

QString SettingsController::shellIntegrationLine(int shell) const
{
    if (!_services.shell.contourBinary)
        return {};
    auto const line =
        shellRcLine(static_cast<config::ShellKind>(shell), _services.shell.contourBinary().path);
    return line ? toQString(*line) : QString {};
}

QString SettingsController::shellIntegrationRcPath(int shell) const
{
    auto const kind = static_cast<config::ShellKind>(shell);
    if (_services.shell.editor == nullptr || config::shellRow(kind) == nullptr)
        return {};
    auto const inspection = inspectShell(kind);
    if (!inspection || inspection->file.empty())
        return {};
    return toQString(displayRcPath(inspection->file, _services.shell.homeDirectory));
}

QVariantMap SettingsController::shellIntegrationConfirm(int shell) const
{
    auto const* const row = config::shellRow(static_cast<config::ShellKind>(shell));
    if (row == nullptr)
        return {};
    auto const rc = shellIntegrationRcPath(shell).toStdString();
    auto const line = shellIntegrationLine(shell).toStdString();
    auto const& words = CardStageRows[std::to_underlying(CardStage::Confirm)];
    return QVariantMap {
        { QStringLiteral("title"), toQString(cardText(words.title, row->name, rc, line)) },
        { QStringLiteral("line"), toQString(cardText(words.body, row->name, rc, line)) },
        { QStringLiteral("primary"), toQString(words.primary) },
        { QStringLiteral("secondary"), toQString(words.secondary) },
    };
}

void SettingsController::installShellIntegration(int shell)
{
    auto const kind = static_cast<config::ShellKind>(shell);
    if (_services.shell.editor == nullptr || config::shellRow(kind) == nullptr)
        return;
    if (auto const block = installBlock(); block != InstallBlock::None)
    {
        emit errorOccurred(toQString(InstallBlockRows[std::to_underlying(block)].text));
        return;
    }
    auto const line = shellIntegrationLine(shell).toStdString();
    if (line.empty())
    {
        emit errorOccurred(
            tr("Shell integration could not be added: the path of this Contour is not usable."));
        return;
    }
    // Named before the write, as the toast names it: the file the editor is about to append to.
    auto const rc = shellIntegrationRcPath(shell).toStdString();
    if (auto const installed = _services.shell.editor->install(kind, line, rcEnvironment()); !installed)
        emit errorOccurred(toQString(shellIntegrationFailedText(rc, installed.error())));
    listShellRows();
}

QVariantList SettingsController::sections() const
{
    auto sections = QVariantList {};
    for (auto const& row: SettingsSectionRows)
    {
        auto groups = QVariantList {};
        for (auto const& group: groupsOf(row.section))
            groups.push_back(QVariantMap {
                { QStringLiteral("id"), static_cast<int>(group.group) },
                { QStringLiteral("title"), toQString(group.title) },
                { QStringLiteral("scope"), scopeToken(group.scope) },
            });
        auto links = QVariantList {};
        for (auto const& link: linksOf(row.section))
            links.push_back(QVariantMap {
                { QStringLiteral("title"), toQString(link.title) },
                { QStringLiteral("target"), toQString(link.id) },
            });
        sections.push_back(QVariantMap {
            { QStringLiteral("id"), toQString(row.id) },
            { QStringLiteral("title"), toQString(row.title) },
            { QStringLiteral("icon"), static_cast<int>(row.icon) },
            { QStringLiteral("groups"), groups },
            { QStringLiteral("links"), links },
        });
    }
    return sections;
}

QVariantList SettingsController::sidebar() const
{
    auto rows = QVariantList {};
    for (auto const& section: SettingsSectionRows)
    {
        rows.push_back(QVariantMap {
            { QStringLiteral("kind"), QStringLiteral("section") },
            { QStringLiteral("id"), toQString(section.id) },
            { QStringLiteral("title"), toQString(section.title) },
            { QStringLiteral("icon"), static_cast<int>(section.icon) },
        });
        // A section that lists entities of its own has their rows beneath it.
        switch (section.section)
        {
            case SettingsSection::Profiles:
                for (auto const& raw: _profiles)
                {
                    auto row = raw.toMap();
                    row[QStringLiteral("kind")] = QStringLiteral("profile");
                    auto const* profile =
                        _config().findProfile(row.value(QStringLiteral("name")).toString().toStdString());
                    auto const tabColor = profile != nullptr ? profile->tabColor.value() : std::nullopt;
                    row[QStringLiteral("tabColor")] = tabColor ? rgbToHex(*tabColor) : QString {};
                    rows.push_back(row);
                }
                break;
            case SettingsSection::Hosts:
                for (auto const& raw: _hosts)
                {
                    auto row = raw.toMap();
                    row[QStringLiteral("kind")] = QStringLiteral("host");
                    rows.push_back(row);
                }
                break;
            case SettingsSection::General:
            case SettingsSection::Appearance:
            case SettingsSection::Keyboard:
            case SettingsSection::ShellIntegration: break;
        }
    }
    rows.push_back(QVariantMap {
        { QStringLiteral("kind"), QStringLiteral("edit") },
        { QStringLiteral("title"), toQString(EditConfigEntryTitle) },
    });
    return rows;
}

QVariantList SettingsController::searchSettings(QString const& query) const
{
    // The open profile's fields, then the globals: the order the page lists them in.
    auto entries = std::vector<SettingsSearchEntry> {};
    if (_profileAccess != DraftAccess::Closed)
        for (auto const& group: profileFieldGroups())
            for (auto const& descriptor: group.fields)
                entries.push_back({ .key = descriptor.key.toStdString(),
                                    .label = descriptor.label.toStdString(),
                                    .help = descriptor.help.toStdString(),
                                    .group = group.group });
    for (auto const& descriptor: globalFieldDescriptors())
        entries.push_back({ .key = descriptor.key.toStdString(),
                            .label = descriptor.label.toStdString(),
                            .help = descriptor.help.toStdString(),
                            .group = descriptor.group });
    // A host's rows, found whether or not a host is open: the page opens one to show them.
    for (auto const& row: HostPageRows)
        entries.push_back({ .key = std::string { row.key },
                            .label = std::string { row.label },
                            .help = std::string { row.help },
                            .group = row.group });

    // The pages the sections link to are found by their title, after the rows.
    for (auto const& link: SettingsPageLinks)
        entries.push_back({ .key = std::string { link.id },
                            .label = std::string { link.title },
                            .help = {},
                            .group = SettingsGroup::GeneralWindows,
                            .target = link.target });

    auto found = QVariantList {};
    for (auto const index: matchSettings(query.toStdString(), entries))
    {
        auto const& entry = entries.at(index);
        if (entry.target)
        {
            auto const& link = SettingsPageLinks.at(static_cast<std::size_t>(*entry.target));
            found.push_back(QVariantMap {
                { QStringLiteral("key"), QString::fromStdString(entry.key) },
                { QStringLiteral("label"), QString::fromStdString(entry.label) },
                { QStringLiteral("section"), toQString(settingsSectionRow(link.section).id) },
                { QStringLiteral("target"), toQString(link.id) },
                { QStringLiteral("scope"), scopeToken(SettingsScope::Global) },
            });
            continue;
        }
        auto const& group = settingsGroupRow(entry.group);
        found.push_back(QVariantMap {
            { QStringLiteral("key"), QString::fromStdString(entry.key) },
            { QStringLiteral("label"), QString::fromStdString(entry.label) },
            { QStringLiteral("section"), toQString(settingsSectionRow(group.section).id) },
            { QStringLiteral("group"), static_cast<int>(entry.group) },
            { QStringLiteral("scope"), scopeToken(group.scope) },
        });
    }
    return found;
}

std::expected<void, SettingsError> SettingsController::setGlobalField(QString const& key,
                                                                      QVariant const& value)
{
    if (locked())
        return lockedRefusal();
    auto const descriptor = std::ranges::find(globalFieldDescriptors(), key, &GlobalFieldDescriptor::key);
    if (descriptor == globalFieldDescriptors().end())
        return refuse(SettingsErrorKind::UnknownSetting, "Unknown global setting.");
    // A refused value emits no change: the model is as it was, and the editor puts the stored value back
    // itself (SettingRow.qml's commitText).
    if (auto const valid =
            descriptor->validate ? descriptor->validate(value) : std::expected<void, std::string> {};
        !valid)
        return refuse(SettingsErrorKind::InvalidValue, valid.error());
    // An override the merge of settings.yml does not apply would be written and then do nothing.
    if (config::globalOverrideFor(key.toStdString()) == nullptr)
        return refuse(SettingsErrorKind::NotApplied,
                      "This setting cannot be saved: settings.yml does not apply it.");
    if (auto const writable = globalDraftWritable(); !writable)
        return writable;
    // What the value is written as comes from the row, not from how its text looks.
    _globalDraft.globalOverrides[key.toStdString()] = descriptor->toYaml(value);
    switch (settingsValueShapeOf(descriptor->type))
    {
        case SettingsValueShape::Scalar: _globalDraft.collectionOverrides.erase(key.toStdString()); break;
        case SettingsValueShape::Collection:
            _globalDraft.collectionOverrides.insert(key.toStdString());
            break;
    }
    return stageGlobalDraft();
}

std::expected<void, SettingsError> SettingsController::resetGlobalField(QString const& key)
{
    if (auto const writable = globalDraftWritable(); !writable)
        return writable;
    _globalDraft.globalOverrides.erase(key.toStdString());
    _globalDraft.collectionOverrides.erase(key.toStdString());
    // A Reset is a click, not a drag: written at once, its row never shows the value it removed while the
    // value contour.yml gives is not known until the reload.
    return stageGlobalDraft().and_then([this] {
        auto flushed = _session->flush();
        emit changesChanged();
        return flushed;
    });
}

std::expected<void, SettingsError> SettingsController::writeGlobalOverride(std::string_view key,
                                                                           std::string_view value)
{
    if (auto const writable = globalDraftWritable(); !writable)
        return writable;
    // "Use New": a side-file profile spells its status line out, so the switch rewrites those that spell the
    // old edition's default, in the same change as settings.yml.
    auto files = std::vector<config::StagedFile> {};
    if (key == config::DefaultsEditionKey)
    {
        // The profiles are rewritten from the configuration's copies: an edit still inside the debounce is
        // written (and loaded) first, or the rewrite would replace it.
        for (auto const& pending: _session->pendingWrites(config::SideFileKind::Profile))
        {
            // The open draft carries its own staged edit; the rewrite below starts from it.
            auto const isOpen =
                _profileAccess == DraftAccess::Editable && pending.name == _editingProfile.toStdString();
            if (!isOpen)
                if (auto const settled = reseededOnFailure(settlePendingOf(pending)); !settled)
                    return settled;
        }
        if (auto const now = config::configEnumFromToken<config::DefaultsEdition>(value))
            files = profilesAdoptingEdition(*now);
    }
    _globalDraft.globalOverrides[std::string { key }] = std::string { value };
    _globalDraft.collectionOverrides.erase(std::string { key }); // A plain value, however it reads.
    files.push_back(
        { .file = config::guiSettingsFile(), .bytes = config::emitGuiSettingsYaml(_globalDraft) });
    if (auto const staged = _session->stageTogether(files); !staged)
    {
        reseedDrafts();
        emit changesChanged();
        return staged;
    }
    emit globalFieldsChanged();
    // Only settings.yml's change (with the side-file profiles staged beside it): an unrelated pending file
    // that cannot be written is that file's own flush to report, not this write's.
    // The caller shows the error; the drafts are re-seeded here, or a write that failed would be staged
    // again by the next edit of either draft.
    auto flushed = reseededOnFailure(_session->flushChangeOf(config::guiSettingsFile()));
    emit changesChanged();
    return flushed;
}

std::vector<config::StagedFile> SettingsController::profilesAdoptingEdition(config::DefaultsEdition now)
{
    auto const before = _config().defaultsEdition;
    auto files = std::vector<config::StagedFile> {};
    for (auto const& [name, profile]: _config().profiles.value())
    {
        if (!isSideFileProfile(name))
            continue;
        auto const isOpen = _profileAccess == DraftAccess::Editable && _editingProfile.toStdString() == name;
        auto adopted = isOpen ? _draft : profile;
        auto statusLine = adopted.statusLine.value();
        auto const indicator = config::indicatorForEdition(statusLine.indicator, before, now);
        if (std::ranges::all_of(config::indicatorSegments(), [&](config::IndicatorSegment const& segment) {
                return indicator.*segment.spelled == statusLine.indicator.*segment.spelled;
            }))
            continue;
        // A profile that is not open is rewritten from the configuration's copy: what the session saw of
        // its file is what the loader read, so a hand edit made since is a conflict.
        if (!isOpen)
            _session->observe(config::profileFile(name), loadedBytesOf(config::profileFile(name)));
        statusLine.indicator = indicator;
        adopted.statusLine = statusLine;
        // The open draft keeps a link pattern in error, which its page shows; the file does not get it.
        files.push_back(
            { .file = config::profileFile(name), .bytes = config::emitProfileYaml(writableForm(adopted)) });
        if (isOpen)
        {
            _draft = std::move(adopted);
            emit draftChanged();
        }
    }
    return files;
}

void SettingsController::commitGlobalField(QString const& key, QVariant const& value)
{
    auto const descriptor = std::ranges::find(globalFieldDescriptors(), key, &GlobalFieldDescriptor::key);
    if (descriptor != globalFieldDescriptors().end() && descriptor->type == SettingsValueType::Shortcut)
    {
        // The recorder's chord: Qt's key code and modifiers, which the row cannot spell as YAML itself.
        auto const chord = value.toMap();
        setHotkey(chord.value(QStringLiteral("key")).toInt(),
                  chord.value(QStringLiteral("modifiers")).toInt());
        return;
    }
    report(setGlobalField(key, value));
}

QString SettingsController::setHotkey(int key, int modifiers)
{
    // What was wrong with the last attempt is no longer: the row shows this one's answer.
    auto const refuse = [this](std::string_view sentence) {
        _hotkeyProblem = QString::fromUtf8(sentence.data(), static_cast<qsizetype>(sentence.size()));
        emit globalFieldsChanged();
        return _hotkeyProblem;
    };
    auto const chord = platform::hotkeyChordOf(key, Qt::KeyboardModifiers::fromInt(modifiers));
    if (!chord)
        return refuse(hotkeyRefusalText(platform::HotkeyError::UnsupportedKey));
    auto const valid = config::validateHotkeyChord(*chord);
    if (!valid)
        return refuse(hotkeyRefusalText(valid.error()));
    if (display::isMenuOwnedChord(*valid, _services.menuBar))
        return refuse(hotkeyRefusalText(platform::HotkeyError::MenuOwned));
    auto const staged = setGlobalField(HotkeyKey, QString::fromStdString(config::hotkeyYaml(*valid)));
    if (!staged)
    {
        report(staged);
        return refuse(staged.error().message);
    }
    _hotkeyProblem.clear();
    emit globalFieldsChanged();
    return {};
}

void SettingsController::clearHotkey()
{
    _hotkeyProblem.clear();
    commitGlobalReset(HotkeyKey);
}

void SettingsController::setHotkeyProblem(QString problem)
{
    if (_hotkeyProblem == problem)
        return;
    _hotkeyProblem = std::move(problem);
    emit globalFieldsChanged();
}

void SettingsController::commitGlobalReset(QString const& key)
{
    report(resetGlobalField(key));
}

// {{{ Color-scheme draft

QVariantList SettingsController::schemeColors() const
{
    auto colors = QVariantList {};
    if (_schemeAccess == DraftAccess::Closed)
        return colors;
    for (auto const& descriptor: schemeColorDescriptors())
    {
        auto row = QVariantMap {};
        row[QStringLiteral("key")] = descriptor.key;
        row[QStringLiteral("label")] = descriptor.label;
        row[QStringLiteral("color")] = rgbToHex(descriptor.get(_schemeDraft));
        row[QStringLiteral("group")] =
            toQString(SchemeColorGroupTokens.at(static_cast<size_t>(descriptor.group)));
        row[QStringLiteral("against")] =
            descriptor.sitsOn ? rgbToHex(descriptor.sitsOn(_schemeDraft)) : QString {};
        row[QStringLiteral("colorSource")] =
            toQString(SchemeColorSourceTokens.at(static_cast<size_t>(descriptor.source(_schemeDraft))));
        colors.push_back(row);
    }
    return colors;
}

QVariantList SettingsController::schemeGallery() const
{
    auto cards = _schemeGalleryCards;
    if (_schemeAccess == DraftAccess::Closed)
        return cards;
    // The scheme being edited is drawn from its draft; one staged and not yet written takes its place in
    // name order, as the written one will.
    auto const card = galleryCardOf(_editingScheme, _schemeAccess == DraftAccess::Editable, _schemeDraft);
    auto const nameOf = [](QVariant const& raw) {
        return raw.toMap().value(QStringLiteral("name")).toString();
    };
    auto const at = std::ranges::find(cards, _editingScheme, nameOf);
    if (at != cards.end())
        *at = card;
    else
        cards.insert(std::ranges::lower_bound(cards,
                                              _editingScheme.toStdString(),
                                              std::less {},
                                              [&](QVariant const& raw) { return nameOf(raw).toStdString(); }),
                     card);
    return cards;
}

QVariantMap SettingsController::contrastReadout(QString const& foreground, QString const& background) const
{
    auto result = QVariantMap {};
    result[QStringLiteral("text")] = QString {};
    result[QStringLiteral("passes")] = false; // QML-facing bool
    if (!QColor::isValidColorName(foreground) || !QColor::isValidColorName(background))
        return result;
    auto const readout = contrastReadoutOf(hexToRgb(foreground), hexToRgb(background));
    result[QStringLiteral("text")] = QString::fromStdString(formatContrast(readout));
    result[QStringLiteral("passes")] = readout.verdict == ContrastVerdict::Passes;
    return result;
}

void SettingsController::editColorScheme(QString const& name)
{
    report(settleOpenScheme());
    report(settlePendingOf(config::colorSchemeFile(name.toStdString())));
    auto const& cfg = _config();
    auto const file = config::colorSchemeFile(name.toStdString());
    // A side file's draft is parsed from the very bytes the session records as seen: the configuration
    // loads a scheme only where a profile names it, and may hold an older version of it.
    auto const read = _services.store->read(file);
    auto const bytes = read ? *read : std::nullopt;
    auto const sideFile = bytes ? config::parseColorSchemeYaml(
                                      cfg.configFile.parent_path() / config::relativePathOf(file), *bytes)
                                : std::nullopt;
    auto palette = vtbackend::ColorPalette {};
    if (sideFile)
        palette = *sideFile;
    else if (auto const it = cfg.colorschemes.value().find(name.toStdString());
             it != cfg.colorschemes.value().end())
        palette = it->second;

    _schemeDraft = palette;
    _editingScheme = name;
    _schemeAccess = !locked() && (sideFile || colorSchemeSideFileExists(name.toStdString()))
                        ? DraftAccess::Editable
                        : DraftAccess::ReadOnly;
    _schemeSeededFrom.reset();
    if (_schemeAccess == DraftAccess::Editable)
    {
        _schemeSeededFrom = bytes;
        _session->observe(file, bytes);
    }
    emit schemeDraftChanged();
    emit editingSchemeChanged();
}

std::expected<void, SettingsError> SettingsController::stageSchemeDraft()
{
    auto bytes = config::emitColorSchemeYaml(_schemeDraft);
    auto staged = stage(config::colorSchemeFile(_editingScheme.toStdString()), bytes);
    if (staged)
        _schemeSeededFrom = std::move(bytes);
    return staged;
}

std::expected<void, SettingsError> SettingsController::setSchemeColor(QString const& key,
                                                                      QString const& color)
{
    if (locked())
        return lockedRefusal();
    switch (_schemeAccess)
    {
        case DraftAccess::Closed: return refuse(SettingsErrorKind::ReadOnly, "No color scheme is open.");
        case DraftAccess::ReadOnly:
            return refuse(SettingsErrorKind::ReadOnly,
                          "This color scheme is not a GUI side file · Duplicate to Edit.");
        case DraftAccess::Editable: break;
    }
    auto const* descriptor = findSchemeColor(key);
    if (descriptor == nullptr)
        return refuse(SettingsErrorKind::UnknownSetting, "Unknown color slot.");
    // A colour equal to the one shown is no edit -- unless the slot is derived: committing the colour it
    // shows then pins it, so that the colour stays when what it derives from changes.
    auto const rgb = hexToRgb(color);
    if (descriptor->source(_schemeDraft) != SchemeColorSource::Derived
        && descriptor->get(_schemeDraft) == rgb)
        return {};
    descriptor->set(_schemeDraft, rgb);
    emit schemeDraftChanged();
    return stageSchemeDraft();
}

void SettingsController::commitSchemeColor(QString const& key, QString const& color)
{
    report(setSchemeColor(key, color));
}

void SettingsController::resetSchemeColor(QString const& key)
{
    auto const* descriptor = findSchemeColor(key);
    if (_schemeAccess != DraftAccess::Editable || descriptor == nullptr
        || descriptor->source(_schemeDraft) != SchemeColorSource::Pinned)
        return;
    descriptor->derived->assign(_schemeDraft, std::nullopt);
    emit schemeDraftChanged();
    report(stageSchemeDraft());
}

namespace
{
    /// Whether @p colors names color scheme @p name, as the one scheme or as either half of the pair.
    [[nodiscard]] bool drawsWithScheme(config::ColorConfig const& colors, std::string const& name)
    {
        if (auto const* const simple = std::get_if<config::SimpleColorConfig>(&colors))
            return simple->colorScheme == name;
        auto const& dual = std::get<config::DualColorConfig>(colors);
        return dual.colorSchemeLight == name || dual.colorSchemeDark == name;
    }

    /// @p names as a sentence's noun phrase: profile “a”, or profiles “a”, “b”.
    [[nodiscard]] std::string joinedNames(std::vector<std::string> const& names)
    {
        auto joined = std::string { names.size() == 1 ? "profile " : "profiles " };
        auto separator = std::string_view {};
        for (auto const& name: names)
        {
            joined += std::format("{}“{}”", separator, name);
            separator = ", ";
        }
        return joined;
    }

    /// @p colors with every use of scheme @p from naming @p to.
    [[nodiscard]] config::ColorConfig withSchemeRenamed(config::ColorConfig colors,
                                                        std::string const& from,
                                                        std::string const& to)
    {
        if (auto* const simple = std::get_if<config::SimpleColorConfig>(&colors))
        {
            if (simple->colorScheme == from)
                simple->colorScheme = to;
        }
        else
            for (auto* const half: { &std::get<config::DualColorConfig>(colors).colorSchemeLight,
                                     &std::get<config::DualColorConfig>(colors).colorSchemeDark })
                if (*half == from)
                    *half = to;
        return colors;
    }
} // namespace

std::expected<std::vector<std::string>, SettingsError> SettingsController::profilesDrawingWith(
    std::string const& name)
{
    // What is still inside the debounce for a profile is written first, so that the configuration says what
    // each draws with. The open draft is among them: it is staged on every edit.
    for (auto const& pending: _session->pendingWrites(config::SideFileKind::Profile))
        if (auto const settled = settlePendingOf(pending); !settled)
            return std::unexpected(settled.error());
    auto users = std::vector<std::string> {};
    for (auto const& [profileName, profile]: _config().profiles.value())
        if (_session->pendingFor(config::profileFile(profileName)) != config::PendingChange::Removal
            && drawsWithScheme(profile.colors.value(), name))
            users.push_back(profileName);
    return users;
}

bool SettingsController::schemeNameTaken(std::string const& name) const
{
    return colorSchemeSideFileExists(name) || isInlineColorScheme(name)
           || _config().colorschemes.value().contains(name)
           || _session->pendingFor(config::colorSchemeFile(name)) == config::PendingChange::Write
           || std::ranges::any_of(_config().colorschemes.value(),
                                  [&](auto const& entry) { return sameNameFolded(name, entry.first); })
           || std::ranges::any_of(
               _session->pendingWrites(config::SideFileKind::ColorScheme),
               [&](config::SideFile const& pending) { return sameNameFolded(name, pending.name); });
}

std::expected<QString, SettingsError> SettingsController::duplicateColorScheme(QString const& name)
{
    if (locked())
        return lockedRefusal();
    if (_editingScheme != name)
        if (auto const settled = settlePendingOf(config::colorSchemeFile(name.toStdString())); !settled)
            return std::unexpected(settled.error());
    auto palette = _schemeAccess != DraftAccess::Closed && _editingScheme == name
                       ? _schemeDraft
                       : schemePalette(name.toStdString());
    auto const copyName = uniqueCopyName(name.toStdString(), [this](std::string_view candidate) {
        return schemeNameTaken(std::string { candidate });
    });
    if (auto const valid = checkNewName(copyName); !valid)
        return std::unexpected(valid.error());
    if (auto const staged = stage(config::colorSchemeFile(copyName), config::emitColorSchemeYaml(palette));
        !staged)
        return std::unexpected(staged.error());
    _schemeDraft = std::move(palette);
    _editingScheme = QString::fromStdString(copyName);
    _schemeAccess = DraftAccess::Editable;
    emit schemeDraftChanged();
    emit editingSchemeChanged();
    return _editingScheme;
}

void SettingsController::duplicateScheme(QString const& name)
{
    report(duplicateColorScheme(name));
}

std::expected<void, SettingsError> SettingsController::renameColorScheme(QString const& oldName,
                                                                         QString const& newName)
{
    if (locked())
        return lockedRefusal();
    if (!colorSchemeSideFileExists(oldName.toStdString()))
        return refuse(SettingsErrorKind::ReadOnly,
                      "Only color schemes made in Settings can be renamed here.");
    auto const trimmed = newName.trimmed();
    if (trimmed == oldName)
        return {}; // nothing to do
    if (auto const valid = checkNewName(trimmed.toStdString()); !valid)
        return valid;
    // Reject a destination naming ANY existing scheme: a side file, a contour.yml inline scheme (which
    // would shadow the written side file at load, leaving a dead file), or a config-known scheme such as
    // the builtin "default".
    if (schemeNameTaken(trimmed.toStdString()))
        return refuse(SettingsErrorKind::NameTaken, "A color scheme with that name already exists.");

    // The profiles that draw with it follow it to its new name -- except one contour.yml defines, which
    // Settings never writes, and which would keep naming a scheme that is gone.
    auto const users = profilesDrawingWith(oldName.toStdString());
    if (!users)
        return std::unexpected(users.error());
    for (auto const& user: *users)
        if (!isSideFileProfile(user))
            return refuse(SettingsErrorKind::InUse,
                          std::format("The color scheme “{}” is used by the profile “{}”, which contour.yml "
                                      "defines, so it cannot be renamed here.",
                                      oldName.toStdString(),
                                      user));

    // The open draft carries what is staged for the scheme; another one is as its side file has it, read
    // once: the palette is parsed from the very bytes the session records as seen, as opening it does.
    auto palette = std::optional<vtbackend::ColorPalette> {};
    if (_schemeAccess != DraftAccess::Closed && _editingScheme == oldName)
        palette = _schemeDraft;
    else
    {
        if (auto const settled = settlePendingOf(config::colorSchemeFile(oldName.toStdString())); !settled)
            return settled;
        auto const file = config::colorSchemeFile(oldName.toStdString());
        auto const read = _services.store->read(file);
        auto const bytes = read ? *read : std::nullopt;
        if (bytes)
        {
            palette = config::parseColorSchemeYaml(
                _config().configFile.parent_path() / config::relativePathOf(file), *bytes);
            _session->observe(file, bytes);
            // A file that does not parse is a hand edit in progress, not a palette to carry over: the
            // configuration's copy is an older version, and the removal below would drop the edit.
            if (!palette)
                return refuse(SettingsErrorKind::ChangedOnDisk,
                              std::format("{} does not parse; fix it or open it before renaming.",
                                          config::relativePathOf(file).generic_string()));
        }
        if (!palette)
            if (auto const it = _config().colorschemes.value().find(oldName.toStdString());
                it != _config().colorschemes.value().end())
                palette = it->second;
    }
    if (!palette)
        return refuse(SettingsErrorKind::UnknownSetting, "Open the color scheme to rename it.");

    // One change: the new file, the profiles that move to it and the old one's removal are written together
    // or not at all, every write before the removal.
    auto files = std::vector<config::StagedFile> {
        { .file = config::colorSchemeFile(trimmed.toStdString()),
          .bytes = config::emitColorSchemeYaml(*palette) },
    };
    auto movedDraft = std::optional<TerminalProfile> {};
    for (auto const& user: *users)
    {
        auto const isOpen = _profileAccess == DraftAccess::Editable && _editingProfile.toStdString() == user;
        auto profile = isOpen ? writableDraft() : *_config().findProfile(user);
        profile.colors =
            withSchemeRenamed(profile.colors.value(), oldName.toStdString(), trimmed.toStdString());
        if (isOpen)
        {
            movedDraft = _draft;
            movedDraft->colors = profile.colors;
        }
        else
            // The bytes below are the configuration's, so what the session saw of the file is what the loader
            // read: a hand edit made since is a conflict.
            _session->observe(config::profileFile(user), loadedBytesOf(config::profileFile(user)));
        files.push_back({ .file = config::profileFile(user), .bytes = config::emitProfileYaml(profile) });
    }
    files.push_back({ .file = config::colorSchemeFile(oldName.toStdString()), .bytes = std::nullopt });
    auto staged = _session->stageTogether(files);
    emit changesChanged();
    if (!staged)
        return staged;
    refreshSchemeList();
    if (movedDraft)
    {
        _draft = std::move(*movedDraft);
        _draftSeededFrom = config::emitProfileYaml(writableDraft());
        emit draftChanged();
    }

    if (_editingScheme == oldName)
    {
        _editingScheme = trimmed;
        emit editingSchemeChanged();
    }
    return {};
}

void SettingsController::requestRenameScheme(QString const& oldName, QString const& newName)
{
    report(renameColorScheme(oldName, newName));
}

std::expected<void, SettingsError> SettingsController::deleteColorScheme(QString const& name)
{
    if (locked())
        return lockedRefusal();
    // Only GUI-created (side-file) schemes can be deleted; a builtin/inline scheme has no side file, and a
    // removal treats an absent file as success, so without this guard a delete of a scheme that is still
    // there would look done. Mirrors deleteProfile's origin guard.
    if (!colorSchemeSideFileExists(name.toStdString()))
        return refuse(SettingsErrorKind::ReadOnly,
                      "Only color schemes made in Settings can be deleted here.");
    // A scheme a profile still draws with would leave it on the default palette: refused, naming them.
    auto const users = profilesDrawingWith(name.toStdString());
    if (!users)
        return std::unexpected(users.error());
    if (!users->empty())
        return refuse(SettingsErrorKind::InUse,
                      std::format("The color scheme “{}” is used by {}; give {} another scheme first.",
                                  name.toStdString(),
                                  joinedNames(*users),
                                  users->size() == 1 ? "it" : "them"));

    // As the gallery last listed it, unless it is the open draft, which was observed when it was opened.
    if (_editingScheme != name)
        if (auto const listed = _schemeListedBytes.find(name.toStdString());
            listed != _schemeListedBytes.end())
            _session->observe(config::colorSchemeFile(name.toStdString()), listed->second);
    if (auto const staged = stage(config::colorSchemeFile(name.toStdString()), std::nullopt); !staged)
        return staged;
    refreshSchemeList();

    if (_editingScheme == name)
    {
        _schemeAccess = DraftAccess::Closed;
        _editingScheme.clear();
        emit schemeDraftChanged();
        emit editingSchemeChanged();
    }
    return {};
}

void SettingsController::requestDeleteScheme(QString const& name)
{
    report(deleteColorScheme(name));
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
    /// at all, and its text -- "#RRGGBB" or a role's name. Split in two because an unset colour is not
    /// black -- it means "inherit" -- and one string cannot say both.
    void writeColorPair(QVariantMap& row,
                        QString const& hasKey,
                        QString const& colorKey,
                        std::optional<vtbackend::StatusLineDefinitions::ItemColor> const& color)
    {
        row[hasKey] = color.has_value();
        row[colorKey] =
            color ? QString::fromStdString(vtbackend::StatusLineDefinitions::formatItemColor(*color))
                  : QString {};
    }

    /// An optional colour from the "has"/value key pair @p hasKey / @p colorKey: read as the template
    /// parser reads it, so a role stays a role and text that is no colour is unset rather than black.
    [[nodiscard]] std::optional<vtbackend::StatusLineDefinitions::ItemColor> optionalColor(
        QVariantMap const& row, QString const& hasKey, QString const& colorKey)
    {
        if (!row.value(hasKey).toBool())
            return std::nullopt;
        return vtbackend::StatusLineDefinitions::parseItemColor(row.value(colorKey).toString().toStdString());
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

        styles.foregroundColor = optionalColor(row, QStringLiteral("hasColor"), QStringLiteral("color"));
        styles.backgroundColor = optionalColor(row, QStringLiteral("hasBgColor"), QStringLiteral("bgColor"));

        auto const flags = row.value(QStringLiteral("flags")).toMap();
        for (auto const& flag: vtbackend::StatusLineDefinitions::CellFlagNames)
            if (flags.value(flagKey(flag)).toBool())
                styles.flags.enable(flag.flag);

        styles.textLeft = row.value(QStringLiteral("textLeft")).toString().toStdString();
        styles.textRight = row.value(QStringLiteral("textRight")).toString().toStdString();
        return styles;
    }

    /// Exposes what @p color paints as, for the editor's preview: "#rrggbb", a role taken as the scheme's
    /// own colour for it (roleColor()); empty while the colour is unset. Deliberately not the bar's
    /// legibility fallback (resolveItemColor()): the preview has no bar under it, and shows which colour the
    /// role names. A QML colour property cannot take a role's name, so the preview reads this beside the
    /// role it keeps for saving.
    void writeColorPreview(QVariantMap& row,
                           QString const& key,
                           std::optional<vtbackend::StatusLineDefinitions::ItemColor> const& color,
                           vtbackend::ColorPalette const& scheme)
    {
        if (!color)
        {
            row[key] = QString {};
            return;
        }
        auto const* const role = std::get_if<vtbackend::StatusLineDefinitions::ColorRole>(&*color);
        row[key] = rgbToHex(role ? vtbackend::StatusLineDefinitions::roleColor(*role, scheme)
                                 : std::get<vtbackend::RGBColor>(*color));
    }

} // namespace

QVariantList SettingsController::parseIndicatorSegment(QString const& templateStr) const
{
    using namespace vtbackend::StatusLineDefinitions;

    auto const scheme = previewPalette();

    auto items = QVariantList {};
    for (auto const& item: vtbackend::parseStatusLineSegment(templateStr.toStdString()))
    {
        std::visit(
            [&items, &scheme](auto const& v) {
                using T = std::decay_t<decltype(v)>;

                auto row = QVariantMap {};
                readStyles(row, v);
                writeColorPreview(row, QStringLiteral("colorPreview"), v.foregroundColor, scheme);
                writeColorPreview(row, QStringLiteral("bgColorPreview"), v.backgroundColor, scheme);
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
                    // Seconds. No control for it yet, but carried so an edit of the segment keeps it.
                    row[QStringLiteral("interval")] = static_cast<int>(v.interval.count());
                    row[QStringLiteral("displayName")] = v.command.empty()
                                                             ? toQString(ItemTraits<T>::Label)
                                                             : QString::fromStdString(v.command);
                }
                else if constexpr (std::same_as<T, Extension>)
                {
                    row[QStringLiteral("name")] = QString::fromStdString(v.name);
                    row[QStringLiteral("displayName")] =
                        v.name.empty() ? toQString(ItemTraits<T>::Label) : QString::fromStdString(v.name);
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
                    row[QStringLiteral("displayName")] = toQString(ItemTraits<T>::Label);
                }
                else if constexpr (std::same_as<T, InputMode>)
                {
                    // `hide` is the mode's template spelling, empty when the item always shows. `chip`
                    // is a QVariantMap bool -- QML reads the look as a yes/no, the documented carve-out.
                    row[QStringLiteral("hide")] =
                        v.hiddenIn ? toQString(inputModeName(*v.hiddenIn)) : QString {};
                    row[QStringLiteral("chip")] = v.look == InputModeLook::Chip;
                    row[QStringLiteral("displayName")] = toQString(ItemTraits<T>::Label);
                }
                else
                    row[QStringLiteral("displayName")] = toQString(ItemTraits<T>::Label);

                // Every item with a column budget, not only Context's: {Cwd:MaxWidth=20} has no control in
                // the page either, and has to survive an edit of its segment all the same.
                if constexpr (HasMaxWidth<T>)
                    row[QStringLiteral("maxWidth")] = unbox<int>(v.maxWidth);

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
            auto item = [&]() -> T {
                if constexpr (std::same_as<T, Text>)
                    return Text { styles, row.value(QStringLiteral("text")).toString().toStdString() };
                else if constexpr (std::same_as<T, Command>)
                {
                    auto command =
                        Command { styles, row.value(QStringLiteral("command")).toString().toStdString() };
                    // A missing or non-positive interval (e.g. a row QML built without one) keeps the
                    // default.
                    if (auto const seconds = row.value(QStringLiteral("interval")).toInt(); seconds > 0)
                        command.interval = std::chrono::seconds { seconds };
                    return command;
                }
                else if constexpr (std::same_as<T, Extension>)
                    return Extension { styles, row.value(QStringLiteral("name")).toString().toStdString() };
                else if constexpr (std::same_as<T, Tabs>)
                {
                    auto const separator = row.value(QStringLiteral("separator")).toString();
                    return Tabs {
                        styles,
                        optionalColor(row, QStringLiteral("hasActiveColor"), QStringLiteral("activeColor")),
                        optionalColor(
                            row, QStringLiteral("hasActiveBackground"), QStringLiteral("activeBackground")),
                        // An empty separator is "unset", so Tabs falls back to its built-in "|" rather than
                        // rendering the tabs run together.
                        separator.isEmpty() ? std::nullopt : std::optional { separator.toStdString() },
                    };
                }
                else if constexpr (std::same_as<T, Context>)
                {
                    auto context = Context { styles };
                    context.verbosity =
                        contextVerbosityFrom(row.value(QStringLiteral("verbosity")).toString().toStdString());
                    auto const separator = row.value(QStringLiteral("separator")).toString();
                    if (!separator.isEmpty())
                        context.separator = separator.toStdString();
                    return context;
                }
                else if constexpr (std::same_as<T, InputMode>)
                {
                    auto inputMode = InputMode { styles };
                    // An unknown or empty mode name always shows, as the template parser reads it.
                    inputMode.hiddenIn =
                        inputModeFrom(row.value(QStringLiteral("hide")).toString().toStdString());
                    if (row.value(QStringLiteral("chip")).toBool())
                        inputMode.look = InputModeLook::Chip;
                    return inputMode;
                }
                else
                    return T { styles };
            }();

            // A missing or nonsensical width keeps the default rather than collapsing the item to nothing.
            if constexpr (HasMaxWidth<T>)
                if (auto const width = row.value(QStringLiteral("maxWidth")).toInt(); width > 0)
                    item.maxWidth = vtbackend::ColumnCount(width);

            segment.emplace_back(std::move(item));
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

QStringList SettingsController::indicatorColorRoles() const
{
    auto roles = QStringList {};
    for (auto const& role: vtbackend::StatusLineDefinitions::ColorRoleNames)
        roles.push_back(toQString(role.name));
    return roles;
}

QString SettingsController::indicatorSegment(int segmentIndex) const
{
    if (_profileAccess == DraftAccess::Closed)
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
    if (_profileAccess != DraftAccess::Editable)
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
    stageProfileDraftReporting();
}

// }}}

// {{{ Link patterns

namespace
{
    using config::HintPatternConfig;

    /// How many rows of SettingsController::linkPatterns the built-ins take, ahead of the profile's own.
    [[nodiscard]] int builtinLinkPatternRows()
    {
        return static_cast<int>(vtbackend::builtinLinkPatternNames().size());
    }

    /// The profile's own patterns among @p entries: those that name no built-in, in their order.
    [[nodiscard]] auto userLinkPatterns(std::vector<HintPatternConfig>& entries)
    {
        return entries | std::views::filter([](HintPatternConfig const& entry) {
                   return !vtbackend::isBuiltinLinkPatternName(entry.name);
               });
    }

    /// The entry of @p entries that user row @p index (counted from the first row after the built-ins'
    /// rows) shows.
    /// @return It, or the end of @p entries for an index out of range.
    [[nodiscard]] std::vector<HintPatternConfig>::iterator userLinkPatternAt(
        std::vector<HintPatternConfig>& entries, int index)
    {
        if (index < 0)
            return entries.end();
        auto users = userLinkPatterns(entries);
        auto const found = std::ranges::next(users.begin(), index, users.end());
        return found == users.end() ? entries.end() : found.base();
    }

    /// A row's `kind`: what the page calls the kind of link a pattern finds.
    [[nodiscard]] QString linkKindName(vtbackend::LinkKind kind)
    {
        switch (kind)
        {
            case vtbackend::LinkKind::Url: return QStringLiteral("url");
            case vtbackend::LinkKind::Path: return QStringLiteral("path");
            case vtbackend::LinkKind::Custom: return QStringLiteral("custom");
        }
        return QStringLiteral("custom");
    }

    /// The regex the last of @p entries named @p name gives that built-in, as the terminal applies them in
    /// order.
    /// @return The regex; nullopt when no entry by that name gives one.
    [[nodiscard]] std::optional<std::string> regexOverrideOf(std::span<HintPatternConfig const> entries,
                                                             std::string_view name)
    {
        auto overrides =
            entries | std::views::reverse | std::views::filter([name](HintPatternConfig const& entry) {
                return entry.name == name && !entry.regex.empty();
            });
        return overrides.empty() ? std::nullopt : std::optional { overrides.front().regex };
    }

    /// The last of @p entries named @p name: the one whose keys win.
    /// @return It, or the end of @p entries when none has that name.
    [[nodiscard]] std::vector<HintPatternConfig>::iterator lastEntryNamed(
        std::vector<HintPatternConfig>& entries, std::string_view name)
    {
        auto const found = std::ranges::find(std::views::reverse(entries), name, &HintPatternConfig::name);
        return found.base() == entries.begin() ? entries.end() : std::prev(found.base());
    }

    /// The messages @p table has for the pattern @p name, one per line; empty when it has none.
    [[nodiscard]] QString diagnosticsOf(vtbackend::LinkPatterns const& table, std::string_view name)
    {
        auto text = QStringList {};
        for (auto const& diagnostic: table.diagnostics())
            if (diagnostic.pattern == name)
                text.push_back(QString::fromStdString(diagnostic.message()));
        return text.join(QLatin1Char('\n'));
    }

    /// What a user row shows as its problem: why the reader would skip the entry, else what compiling it
    /// reports.
    [[nodiscard]] QString problemOf(HintPatternConfig const& entry, vtbackend::LinkPatterns const& table)
    {
        if (auto const reason = config::skippedHintPatternReason(entry))
            return QString::fromStdString(*reason);
        return diagnosticsOf(table, entry.name);
    }

    /// The length, in UTF-16 code units, of the first @p bytes bytes of @p text: the offset QML counts in.
    [[nodiscard]] int utf16Offset(std::string_view text, std::size_t bytes)
    {
        return static_cast<int>(toQString(text.substr(0, bytes)).size());
    }
} // namespace

QVariantList SettingsController::linkPatterns() const
{
    auto rows = QVariantList {};
    if (_profileAccess == DraftAccess::Closed)
        return rows;
    auto const table = config::linkPatternsFor(_draft);
    auto const& entries = _draft.hintPatterns.value();
    for (auto const name: vtbackend::builtinLinkPatternNames())
    {
        // Every built-in compiles on its own (LinkPatterns' tests hold them to it), and the rows the
        // mutators count are one per name.
        auto const* builtin = vtbackend::LinkPatterns::builtins()->find(name);
        Require(builtin != nullptr);
        // As the terminal has it: the compiled table, which applies the entries by its name in order. A
        // broken override leaves the built-in out of it: the row shows that override's regex, finds
        // nothing, and says why.
        auto const* effective = table->find(name);
        auto row = QVariantMap {};
        row[QStringLiteral("name")] = toQString(name);
        row[QStringLiteral("regex")] = QString::fromStdString(
            effective ? effective->source : regexOverrideOf(entries, name).value_or(builtin->source));
        row[QStringLiteral("clickable")] =
            effective != nullptr && effective->activation == vtbackend::LinkActivation::Clickable;
        row[QStringLiteral("url")] = QString {};
        row[QStringLiteral("builtin")] = true;
        row[QStringLiteral("kind")] = linkKindName(builtin->kind);
        row[QStringLiteral("problem")] = diagnosticsOf(*table, name);
        rows.push_back(row);
    }
    for (auto const& entry: entries)
    {
        if (vtbackend::isBuiltinLinkPatternName(entry.name))
            continue; // shown on its built-in's row
        auto row = QVariantMap {};
        row[QStringLiteral("name")] = QString::fromStdString(entry.name);
        row[QStringLiteral("regex")] = QString::fromStdString(entry.regex);
        row[QStringLiteral("clickable")] = entry.clickable.value_or(false);
        row[QStringLiteral("url")] = QString::fromStdString(entry.url.value_or(std::string {}));
        row[QStringLiteral("builtin")] = false;
        row[QStringLiteral("kind")] = linkKindName(vtbackend::LinkKind::Custom);
        row[QStringLiteral("problem")] = problemOf(entry, *table);
        rows.push_back(row);
    }
    return rows;
}

void SettingsController::addLinkPattern()
{
    if (_profileAccess != DraftAccess::Editable)
        return;
    auto patterns = _draft.hintPatterns.value();
    auto const isTaken = [&patterns](std::string const& name) {
        return std::ranges::find(patterns, name, &HintPatternConfig::name) != patterns.end();
    };
    auto freeNames = std::views::iota(1)
                     | std::views::transform([](int number) { return std::format("pattern{}", number); })
                     | std::views::filter(std::not_fn(isTaken));
    patterns.push_back(
        HintPatternConfig { .name = *freeNames.begin(), .regex = {}, .clickable = true, .url = {} });
    _draft.hintPatterns = std::move(patterns);
    emit draftChanged();
    stageProfileDraftReporting();
}

void SettingsController::setLinkPattern(int index, QVariantMap const& row)
{
    if (_profileAccess != DraftAccess::Editable || index < 0)
        return;
    auto patterns = _draft.hintPatterns.value();
    auto const clickable = row.value(QStringLiteral("clickable")).toBool();
    if (index < builtinLinkPatternRows())
    {
        // A built-in keeps its regex: its row writes `clickable` alone, as a name-only override.
        auto const name = vtbackend::builtinLinkPatternNames()[static_cast<std::size_t>(index)];
        auto const* builtin = vtbackend::LinkPatterns::builtins()->find(name);
        auto const byDefault =
            builtin != nullptr && builtin->activation == vtbackend::LinkActivation::Clickable;
        // The terminal merges the entries by its name, in order, each changing the keys it gives: cleared on
        // every one, then given to the last, the one whose keys win, only where it differs from the default.
        auto const named = [name](HintPatternConfig const& entry) {
            return entry.name == name;
        };
        for (auto& entry: patterns | std::views::filter(named))
            entry.clickable = std::nullopt;
        if (clickable != byDefault)
        {
            auto entry = lastEntryNamed(patterns, name);
            if (entry == patterns.end())
                entry = patterns.insert(
                    patterns.end(),
                    HintPatternConfig {
                        .name = std::string { name }, .regex = {}, .clickable = {}, .url = {} });
            entry->clickable = clickable;
        }
        // An entry that now changes nothing goes.
        std::erase_if(patterns, [&named](HintPatternConfig const& entry) {
            return named(entry) && entry.regex.empty() && !entry.clickable && !entry.url;
        });
    }
    else
    {
        auto const entry = userLinkPatternAt(patterns, index - builtinLinkPatternRows());
        if (entry == patterns.end())
            return;
        auto const name = row.value(QStringLiteral("name")).toString().trimmed().toStdString();
        // Renamed after a built-in or another pattern, it would merge into that one's row.
        if (name != entry->name
            && (vtbackend::isBuiltinLinkPatternName(name)
                || std::ranges::find(patterns, name, &HintPatternConfig::name) != patterns.end()))
        {
            fail(std::format("A link pattern named '{}' exists already; choose another name.", name));
            emit draftChanged(); // the row shows its name again, not the refused one
            return;
        }
        auto const url = row.value(QStringLiteral("url")).toString().toStdString();
        entry->name = name;
        entry->regex = row.value(QStringLiteral("regex")).toString().toStdString();
        entry->clickable = clickable;
        entry->url = url.empty() ? std::nullopt : std::optional { url };
    }
    _draft.hintPatterns = std::move(patterns);
    emit draftChanged();
    stageProfileDraftReporting();
}

void SettingsController::removeLinkPattern(int index)
{
    if (_profileAccess != DraftAccess::Editable || index < builtinLinkPatternRows())
        return;
    auto patterns = _draft.hintPatterns.value();
    auto const entry = userLinkPatternAt(patterns, index - builtinLinkPatternRows());
    if (entry == patterns.end())
        return;
    patterns.erase(entry);
    _draft.hintPatterns = std::move(patterns);
    emit draftChanged();
    stageProfileDraftReporting();
}

QVariantMap SettingsController::previewLinkPattern(QVariantMap const& row, QString const& sample) const
{
    auto const name = row.value(QStringLiteral("name")).toString().toStdString();
    auto const regex = row.value(QStringLiteral("regex")).toString().toStdString();
    auto const url = row.value(QStringLiteral("url")).toString().toStdString();
    // The row on its own, as the terminal would compile it: a built-in's row carries the regex in effect.
    auto const entries = std::array { vtbackend::LinkPatternSpec {
        .name = name,
        .regex = regex.empty() ? std::nullopt : std::optional { regex },
        .activation = row.value(QStringLiteral("clickable")).toBool() ? vtbackend::LinkActivation::Clickable
                                                                      : vtbackend::LinkActivation::HintOnly,
        .urlTemplate = url.empty() ? std::nullopt : std::optional { url },
    } };
    auto const table = vtbackend::LinkPatterns::compile(entries);

    auto result = QVariantMap {};
    result[QStringLiteral("problem")] = diagnosticsOf(table, name);
    result[QStringLiteral("note")] = QString {};
    auto spans = QVariantList {};
    auto const* pattern = table.find(name);
    if (pattern != nullptr)
    {
        auto const text = sample.toStdString();
        auto const rows = std::array { vtbackend::makePlainScanRow(text) };
        auto const scan = vtbackend::ScanText { rows, vtbackend::ScanClip {} };
        auto const patternIndex = static_cast<std::size_t>(pattern - table.patterns().data());
        auto const clickable = pattern->activation == vtbackend::LinkActivation::Clickable;
        auto anyRefused = false;
        for (auto const& candidate: vtbackend::findLinks(
                 scan, table, vtbackend::PatternSelection::All, vtbackend::LinkOverlap::Keep))
            if (candidate.patternIndex == patternIndex)
            {
                // A match the terminal would not make a link (§7.6), shown as such, as it underlines nothing.
                auto const refused = clickable && candidate.opening == vtbackend::LinkOpening::Refused;
                anyRefused = anyRefused || refused;
                auto span = QVariantMap {};
                span[QStringLiteral("start")] = utf16Offset(text, candidate.byteBegin);
                span[QStringLiteral("end")] = utf16Offset(text, candidate.byteEnd);
                span[QStringLiteral("target")] = QString::fromStdString(candidate.target);
                span[QStringLiteral("refused")] = refused;
                spans.push_back(span);
            }
        if (pattern->kind == vtbackend::LinkKind::Path)
            result[QStringLiteral("note")] =
                tr("Paths are underlined in the terminal once they are found to exist.");
        else if (anyRefused)
            result[QStringLiteral("note")] =
                tr("A struck-out match is no link: the printed text names no scheme Contour opens.");
    }
    result[QStringLiteral("spans")] = spans;
    return result;
}

// }}}

} // namespace contour::window
