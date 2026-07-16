// SPDX-License-Identifier: Apache-2.0
#include <contour/SettingsController.h>

#include <text_shaper/font.h>

#include <QtCore/QStringList>
#include <QtGui/QColor>

#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <set>
#include <utility>

namespace contour
{

using config::SettingsOrigin;
using config::TerminalProfile;

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

    /// The editable scalar profile fields. Data-driven: it grows one row at a time toward full parity
    /// without touching any other code (the QML renders each row by its `type`).
    std::vector<ProfileFieldDescriptor> const& profileFieldDescriptors()
    {
        using vtbackend::LineCount;
        using vtbackend::LineOffset;
        using Ms = std::chrono::milliseconds;
        static auto const descriptors = std::vector<ProfileFieldDescriptor> {
            // {{{ Appearance / window
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
            { "tab_bar_position",
              "Tab bar position",
              "Where the tab strip sits relative to the terminal content.",
              "enum",
              [](TerminalProfile const& p) {
                  return QVariant(p.tabBarPosition.value() == config::TabBarPosition::Bottom ? "Bottom"
                                                                                             : "Top");
              },
              [](TerminalProfile& p, QVariant const& v) {
                  p.tabBarPosition =
                      v.toString() == "Bottom" ? config::TabBarPosition::Bottom : config::TabBarPosition::Top;
              },
              { "Top", "Bottom" } },
            { "tab_bar_visibility",
              "Tab bar visibility",
              "When the tab strip is shown.",
              "enum",
              [](TerminalProfile const& p) {
                  switch (p.tabBarVisibility.value())
                  {
                      case config::TabBarVisibility::Never: return QVariant("Never");
                      case config::TabBarVisibility::Multiple: return QVariant("Multiple");
                      case config::TabBarVisibility::Always: break;
                  }
                  return QVariant("Always");
              },
              [](TerminalProfile& p, QVariant const& v) {
                  auto const s = v.toString();
                  if (s == "Never")
                      p.tabBarVisibility = config::TabBarVisibility::Never;
                  else if (s == "Multiple")
                      p.tabBarVisibility = config::TabBarVisibility::Multiple;
                  else
                      p.tabBarVisibility = config::TabBarVisibility::Always;
              },
              { "Always", "Never", "Multiple" } },
            // }}}
            // {{{ Font
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
                  p.fonts.value().size = text::font_size { v.toDouble() };
              } },
            boolField("draw_bold_text_with_bright_colors",
                      "Bold text uses bright colors",
                      "Render bold text using the bright ANSI colors.",
                      [](auto& p) -> auto& { return p.drawBoldTextWithBrightColors; }),
            // }}}
            // {{{ Scrolling
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
              [](TerminalProfile& p, QVariant const& v) { p.modalCursorScrollOff = LineCount(v.toInt()); } },
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
            // }}}
            // {{{ Behavior / timings
            boolField("escape_sandbox",
                      "Escape sandbox",
                      "Escape a Flatpak/Snap sandbox when spawning.",
                      [](auto& p) -> auto& { return p.escapeSandbox; }),
            boolField("search_mode_switch",
                      "Search mode switch",
                      "Switch to Vi normal mode when a search starts.",
                      [](auto& p) -> auto& { return p.searchModeSwitch; }),
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
              [](TerminalProfile& p, QVariant const& v) { p.screenTransitionDuration = Ms { v.toInt() }; } },
            // }}}
        };
        return descriptors;
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
                                       std::shared_ptr<GuiConfigStore> store,
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
    for (auto const& descriptor: profileFieldDescriptors())
    {
        auto row = QVariantMap {};
        row[QStringLiteral("key")] = descriptor.key;
        row[QStringLiteral("label")] = descriptor.label;
        row[QStringLiteral("help")] = descriptor.help;
        row[QStringLiteral("type")] = descriptor.type;
        row[QStringLiteral("value")] = descriptor.get(_draft);
        row[QStringLiteral("options")] = descriptor.options;
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
    for (auto const& descriptor: profileFieldDescriptors())
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
        row[QStringLiteral("options")] = QStringList {};
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

} // namespace contour
