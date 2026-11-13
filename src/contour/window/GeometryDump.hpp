// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QTimer>

#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

class QQuickWindow;

namespace contour::window
{

/// The environment variable that turns the dump on, naming the file it writes. Test-only: the e2e-display
/// workflows (src/contour/test/e2e/sidebar-workflows.py) read where the rows, tabs and panes are from it, so
/// their real pointer events land on the item they mean without a pixel position written into the test.
inline constexpr auto GeometryDumpVariable = std::string_view { "CONTOUR_TEST_GEOMETRY_FILE" };

/// @param value GeometryDumpVariable's value; empty when it is unset.
/// @return The file the dump writes; none, and so no dump, for an empty value.
[[nodiscard]] std::optional<std::filesystem::path> geometryDumpFileOf(QString const& value);

/// The properties a dump reports of an item that has them: what a test tells one row, tab or button from
/// another by. One row per property, so a test that needs another adds it here.
inline constexpr auto GeometryDumpProperties = std::array<char const*, 15> {
    "title",     "tabTitle", "kind", "index",       "tabIndex",     "depth",  "expansion",       "selected",
    "tabActive", "checked",  "text", "activeFocus", "containsDrag", "opened", "keyboardFocused",
};

/// Describes, as one line of JSON, every visible item of @p windows that has an object name, and every
/// visible button (a menu's items have none), but a secret one -- a field that masks what is typed (an
/// `echoMode` other than Normal, such as the askpass dialog's) or an item whose `secret` property is true:
/// `{"windows":[{"title","x","y","width","height","active","items":[{"name",
/// "class","x","y","width","height","props":{...}}]}]}`, items in tree order, rectangles in screen
/// coordinates. An item that is not visible is left out together with everything inside it.
/// @param windows The windows to describe; a null entry is skipped.
/// @return The JSON text.
[[nodiscard]] QByteArray describeWindowItems(std::span<QQuickWindow* const> windows);

/// Writes describeWindowItems() of the app's windows to a file, whenever it changed, on a timer: the
/// e2e-display tests' view of the screen. Constructed only when GeometryDumpVariable is set; it reads nothing
/// of the windows beyond what describeWindowItems() does, and changes nothing.
class GeometryDump: public QObject
{
    Q_OBJECT

  public:
    /// The windows to describe, asked anew on every tick.
    using WindowSource = std::function<std::vector<QQuickWindow*>()>;

    /// Starts the timer; the first dump is written on its first tick.
    /// @param file Where the dump goes. Replaced whole (QSaveFile), so a reader never sees half of one, and
    ///             readable by its owner alone.
    /// @param interval How often the windows are described.
    /// @param windows The windows to describe.
    GeometryDump(std::filesystem::path file, std::chrono::milliseconds interval, WindowSource windows);

  private:
    void tick();

    std::filesystem::path _file;
    WindowSource _windows;
    QByteArray _written;
    QTimer _timer;
};

} // namespace contour::window
