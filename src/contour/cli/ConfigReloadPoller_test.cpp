// SPDX-License-Identifier: Apache-2.0
#include <contour/cli/ConfigReloadPoller.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <expected>
#include <filesystem>
#include <map>
#include <memory>
#include <string>

#include <vthost/testing/FakeProfileCatalog.hpp>
#include <vthost/testing/ForwardingFileSystem.hpp>

using contour::cli::ConfigReloadPoller;
using contour::cli::ReloadOutcome;
using namespace std::chrono_literals;

namespace
{

/// A file system whose every file was last written at @ref time.
struct TimedFileSystem final: vthost::testing::ForwardingFileSystem
{
    std::filesystem::file_time_type time {};

    [[nodiscard]] std::expected<std::filesystem::file_time_type, std::string> lastWriteTime(
        std::filesystem::path const& /*path*/) const override
    {
        return time;
    }
};

/// @return A catalog with one profile, "main".
std::shared_ptr<vthost::ProfileCatalog const> someCatalog()
{
    return std::make_shared<vthost::testing::FakeProfileCatalog>(
        std::map<std::string, vthost::ResolvedProfile> {
            { "main", vthost::testing::fakeProfile("/bin/zsh", vtbackend::LineCount(1000)) } },
        "main");
}

} // namespace

TEST_CASE("the poller reloads only when the file changed", "[cli][reload]")
{
    auto files = TimedFileSystem {};
    files.time = std::filesystem::file_time_type {} + 1h; // T0, when the daemon started
    auto loads = 0;
    auto poller = ConfigReloadPoller { files, "/home/u/.config/contour/contour.yml", [&] {
                                          ++loads;
                                          return std::expected<std::shared_ptr<vthost::ProfileCatalog const>,
                                                               std::string> { someCatalog() };
                                      } };

    auto const unchanged = poller.poll();
    CHECK(unchanged.first == ReloadOutcome::Unchanged);
    CHECK(unchanged.second == nullptr);
    CHECK(loads == 0);

    files.time += 1s; // T1
    auto const reloaded = poller.poll();
    CHECK(reloaded.first == ReloadOutcome::Reloaded);
    CHECK(reloaded.second != nullptr);

    CHECK(poller.poll().first == ReloadOutcome::Unchanged);
    CHECK(loads == 1);
}

TEST_CASE("a configuration that fails to load keeps the old catalog until the next change", "[cli][reload]")
{
    auto files = TimedFileSystem {};
    auto loads = 0;
    auto poller =
        ConfigReloadPoller { files, "contour.yml", [&] {
                                ++loads;
                                return std::expected<std::shared_ptr<vthost::ProfileCatalog const>,
                                                     std::string> { std::unexpected { "bad yaml" } };
                            } };

    files.time += 1s;
    auto const failed = poller.poll();
    CHECK(failed.first == ReloadOutcome::Failed);
    CHECK(failed.second == nullptr);
    CHECK(poller.poll().first == ReloadOutcome::Unchanged); // no retry of the same file
    CHECK(loads == 1);

    files.time += 1s;
    CHECK(poller.poll().first == ReloadOutcome::Failed); // the next change is tried again
    CHECK(loads == 2);
}
