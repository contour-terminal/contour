// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The Settings window's field descriptors: what each editable profile and global setting is called, how it
/// is read and written, and which group it is shown in. Moved out of SettingsController, which interprets
/// them.

#include <contour/config/Config.hpp>
#include <contour/display/GpuInventory.hpp>
#include <contour/window/SettingsSections.hpp>

#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVariant>

#include <expected>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace contour::window::fields
{

/// A data-driven descriptor for one editable scalar profile field: adding a field to the settings
/// page is adding a row here, not editing logic in three places.
struct ProfileFieldDescriptor
{
    QString key;   //!< Stable identifier / QML key.
    QString label; //!< Human-readable label.
    QString help;  //!< One-line help text.
    QString type;  //!< "bool" | "int" | "double" | "string" | "enum".
    std::function<QVariant(config::TerminalProfile const&)> get;        //!< Reads the field as a QVariant.
    std::function<void(config::TerminalProfile&, QVariant const&)> set; //!< Writes the field from QVariant.
    QStringList options {}; //!< For "enum": the allowed values (the combo model + accepted set).
    /// What is wrong with a value, shown under the editor; empty when nothing is. Absent for a field
    /// whose every value is usable.
    std::function<QString(QVariant const&)> validate {};
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
    /// The group: its title and its section are its row of SettingsGroupRows.
    SettingsGroup group = SettingsGroup::ProfileWindow;
    QString glyph;                              //!< Leading pictograph for the heading.
    std::vector<ProfileFieldDescriptor> fields; //!< The fields shown under it, in order.
};

/// One combo-box entry: what is stored, and what is shown.
struct EnumOption
{
    QString value;
    QString label;
};

/// What a runtime options provider may consult.
struct OptionContext
{
    config::Config const& config;
    std::span<display::GpuCandidate const> gpus;
};

/// Checks an edited global value before it is written, and says why when it refuses it.
using GlobalFieldValidator = std::function<std::expected<void, std::string>(QVariant const&)>;

/// A data-driven descriptor for one editable global (application-scope) setting. `toYaml` turns the
/// edited value into the YAML scalar written to settings.yml; the load-time merge re-applies it
/// through the typed per-key loader, so this side needs no parsing.
struct GlobalFieldDescriptor
{
    QString key; //!< contour.yml key path: "reflow_on_resize", or "gutter.line_numbers" for a leaf.
    QString label;
    QString help;
    QString type;
    std::function<QVariant(config::Config const&)> get;
    std::function<std::string(QVariant const&)> toYaml;
    /// Reads an override back from settings.yml as the loader reads it, as the row's value: an invalid
    /// QVariant where the loader would refuse it. A hand-edited override may be spelled otherwise than
    /// toYaml spells it -- `True`, `0x10`, `Dark` -- and still take.
    std::function<QVariant(std::string const&)> fromYaml;
    QStringList options {}; //!< For "enum": the allowed values (the combo model + accepted set).
    /// For "enum" rows whose options are only known at runtime; overrides `options` when set.
    std::function<std::vector<EnumOption>(OptionContext const&)> optionsProvider {};
    GlobalFieldValidator validate {}; //!< Refuses a value the loader would reject; empty accepts all.
    /// The group the row is shown in; set from GlobalFieldGroups by globalFieldDescriptors().
    SettingsGroup group = SettingsGroup::GeneralWindows;
};

/// The editable scalar profile fields, in the groups the settings page shows them under.
/// @return The groups, in page order; built once.
[[nodiscard]] std::vector<ProfileFieldGroup> const& profileFieldGroups();

/// The editable global settings, each with its group.
/// @return The rows, in page order; built once.
[[nodiscard]] std::vector<GlobalFieldDescriptor> const& globalFieldDescriptors();

/// @param text UTF-8 text.
/// @return It as a QString.
[[nodiscard]] QString toQString(std::string_view text);

} // namespace contour::window::fields
