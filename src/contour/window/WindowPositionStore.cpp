// SPDX-License-Identifier: Apache-2.0
#include <contour/config/AtomicFileWrite.hpp>
#include <contour/platform/QtPath.hpp>
#include <contour/window/WindowPositionStore.hpp>

#include <yaml-cpp/yaml.h>

#include <QtCore/QFile>

#include <algorithm>

namespace contour::window
{
QPoint reachableWindowPosition(QPoint requested, QSize size, QRect available, QMargins frameMargins)
{
    auto const left = frameMargins.left();
    auto const top = frameMargins.top();
    auto const right = std::max(left, available.width() - size.width() - frameMargins.right());
    auto const bottom = std::max(top, available.height() - size.height() - frameMargins.bottom());
    return available.topLeft()
           + QPoint(std::clamp(requested.x(), left, right), std::clamp(requested.y(), top, bottom));
}

std::expected<std::optional<WindowPosition>, std::string> FileWindowPositionStore::load(
    std::filesystem::path const& path) const
{
    auto file = QFile(platform::toQString(path));
    if (!file.exists())
        return std::optional<WindowPosition> {};
    if (!file.open(QIODevice::ReadOnly))
        return std::unexpected(file.errorString().toStdString());
    try
    {
        auto const document = YAML::Load(file.readAll().toStdString());
        auto const position = QPoint(document["x"].as<int>(), document["y"].as<int>());
        auto const monitor = document["monitor"].as<int>();
        if (monitor < 0)
            return std::unexpected("Invalid monitor index.");
        return WindowPosition { .position = position, .monitor = monitor };
    }
    catch (YAML::Exception const& error)
    {
        return std::unexpected(error.what());
    }
}

std::expected<void, std::string> FileWindowPositionStore::save(std::filesystem::path const& path,
                                                               WindowPosition const& position)
{
    auto yaml = YAML::Emitter {};
    yaml << YAML::BeginMap << YAML::Key << "x" << YAML::Value << position.position.x() << YAML::Key << "y"
         << YAML::Value << position.position.y() << YAML::Key << "monitor" << YAML::Value << position.monitor
         << YAML::EndMap;
    // A bare --config contour.yml leaves an empty parent; the shared writer requires a directory.
    auto const destination = path.has_parent_path() ? path : std::filesystem::path(".") / path;
    return config::atomicWriteFile(destination, yaml.c_str());
}
} // namespace contour::window
