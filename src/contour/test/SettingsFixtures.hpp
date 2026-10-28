// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A SettingsController over a contour.yml of its own, shared by the settings page and Settings window tests.

#include <contour/config/Config.hpp>
#include <contour/config/GuiConfigStore.hpp>
#include <contour/test/ManualSaveScheduler.hpp>
#include <contour/test/RecordingShellRcEditor.hpp>
#include <contour/window/SettingsController.hpp>

#include <core/platform/FileSystem.hpp>
#include <core/platform/NativeFileSystem.hpp>

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QTemporaryDir>

#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
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
    /// The shells' startup files, scripted and recorded: no test reaches a real rc file. Declared before the
    /// controller, which borrows it.
    RecordingShellRcEditor shellEditor;
    /// Whether the Install rows' binary outlives this process, and the host they run on: a test sets these
    /// before it lists the rows.
    window::BinaryReach binaryReach = window::BinaryReach::Reachable;
    config::HostPlatform shellHost = config::HostPlatform::Other;
    std::unique_ptr<window::SettingsController> controller;
    QStringList errors; ///< What the controller reported through errorOccurred, in order.

    /// The binary the Install rows' line runs.
    static constexpr std::string_view ContourBinary = "/opt/contour/bin/contour";

    /// Makes the store the controller writes through, over the configuration directory.
    using StoreFactory = std::function<std::shared_ptr<config::GuiConfigStore>(std::filesystem::path const&)>;

    /// @param yaml       The contour.yml the controller edits.
    /// @param writeStore Where the controller writes; null writes the side files beside that contour.yml.
    ControllerFixture(std::string_view yaml, std::shared_ptr<config::GuiConfigStore> writeStore):
        ControllerFixture { yaml,
                            [this, writeStore = std::move(writeStore)](std::filesystem::path const& directory)
                                -> std::shared_ptr<config::GuiConfigStore> {
                                if (writeStore)
                                    return writeStore;
                                return std::make_shared<config::FileGuiConfigStore>(fileSystem, directory);
                            } }
    {
    }

    /// @param yaml  The contour.yml the controller edits.
    /// @param files The file system the side files are written through, beside that contour.yml: a
    ///              FaultyFileSystem for a directory the user cannot write. Borrowed: it outlives the
    ///              fixture.
    ControllerFixture(std::string_view yaml, core::platform::FileSystem const& files):
        ControllerFixture { yaml, [&files](std::filesystem::path const& directory) {
                               return std::make_shared<config::FileGuiConfigStore>(files, directory);
                           } }
    {
    }

    /// @param yaml      The contour.yml the controller edits.
    /// @param makeStore Makes the store the controller writes through.
    ControllerFixture(std::string_view yaml, StoreFactory const& makeStore)
    {
        configDir = std::filesystem::path(dir.path().toStdU16String());
        configPath = configDir / "contour.yml";
        {
            auto out = std::ofstream(configPath);
            out << yaml;
        }
        config::loadConfigFromFile(cfg, configPath);
        store = makeStore(configDir);
        controller = std::make_unique<window::SettingsController>(
            [this]() -> config::Config const& { return cfg; },
            window::SettingsControllerServices {
                .store = store,
                // As the app's: reload, and refresh the page from what was reloaded.
                .reload = [this]() -> std::expected<void, std::string> {
                    reload();
                    return {};
                },
                .gpuInventory = nullptr,
                .scheduler = scheduler,
                .shell = { .editor = &shellEditor,
                           .contourBinary =
                               [this] {
                                   return window::LocatedBinary { .path =
                                                                      std::filesystem::path { ContourBinary },
                                                                  .reach = binaryReach };
                               },
                           .host = shellHost,
                           .homeDirectory = "/home/me" },
            },
            vtbackend::ColorPreference::Dark);
        QObject::connect(controller.get(),
                         &window::SettingsController::errorOccurred,
                         [this](QString const& message) { errors.push_back(message); });
    }

    /// Reloads contour.yml and its side files from disk and refreshes the controller, as a reload does.
    void reload()
    {
        cfg = config::Config {};
        config::loadConfigFromFile(cfg, configPath);
        controller->refresh();
    }
};

} // namespace contour::test
