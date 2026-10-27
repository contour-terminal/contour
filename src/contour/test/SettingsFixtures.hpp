// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A SettingsController over a contour.yml of its own, shared by the settings page and Settings window tests.

#include <contour/config/Config.hpp>
#include <contour/config/GuiConfigStore.hpp>
#include <contour/test/ManualSaveScheduler.hpp>
#include <contour/window/SettingsController.hpp>

#include <core/platform/NativeFileSystem.hpp>

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QTemporaryDir>

#include <expected>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string_view>
#include <utility>

namespace contour::test
{

/// A SettingsController over a contour.yml of its own, wired as production wires it: side files are written
/// beside that contour.yml, and a written change reloads it. The debounce is fired by hand. The loader reads
/// the real disk, so the fixture's files are real files in a temporary directory.
struct ControllerFixture
{
    core::platform::NativeFileSystem fileSystem; ///< What the store writes through: the fixture's own files.
    QTemporaryDir dir;
    config::Config cfg;
    std::filesystem::path configDir;
    std::filesystem::path configPath;
    std::shared_ptr<config::GuiConfigStore> store;
    /// The debounce, fired by hand: fire() writes what was committed.
    std::shared_ptr<ManualSaveScheduler> scheduler = std::make_shared<ManualSaveScheduler>();
    std::unique_ptr<window::SettingsController> controller;
    QStringList errors; ///< What the controller reported through errorOccurred, in order.

    /// @param yaml       The contour.yml the controller edits.
    /// @param writeStore Where the controller writes; null writes the side files beside that contour.yml.
    ControllerFixture(std::string_view yaml, std::shared_ptr<config::GuiConfigStore> writeStore)
    {
        configDir = std::filesystem::path(dir.path().toStdU16String());
        configPath = configDir / "contour.yml";
        {
            auto out = std::ofstream(configPath);
            out << yaml;
        }
        config::loadConfigFromFile(cfg, configPath);
        store = writeStore ? std::move(writeStore)
                           : std::make_shared<config::FileGuiConfigStore>(fileSystem, configDir);
        controller = std::make_unique<window::SettingsController>(
            [this]() -> config::Config const& { return cfg; },
            window::SettingsControllerServices {
                .store = store,
                // As the app's: reload, and refresh the page from what was reloaded.
                .reload = [this]() -> std::expected<void, std::string> {
                    cfg = config::Config {};
                    config::loadConfigFromFile(cfg, configPath);
                    controller->refresh();
                    return {};
                },
                .gpuInventory = nullptr,
                .scheduler = scheduler,
            },
            vtbackend::ColorPreference::Dark);
        QObject::connect(controller.get(),
                         &window::SettingsController::errorOccurred,
                         [this](QString const& message) { errors.push_back(message); });
    }
};

} // namespace contour::test
