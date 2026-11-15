// SPDX-License-Identifier: Apache-2.0
#include <core/testing/SuppressWindowsDialogs.hpp>

#define CATCH_CONFIG_RUNNER
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <orchestrator/core/Fixtures.hpp>

namespace
{

[[nodiscard]] std::filesystem::path fixturePath(orchestrator::FixtureRoot root, std::string_view relativePath)
{
    switch (root)
    {
        case orchestrator::FixtureRoot::Contract:
            return std::filesystem::path(ORCHESTRATOR_CONTRACT_DIR) / relativePath;
        case orchestrator::FixtureRoot::Testdata:
            return std::filesystem::path(ORCHESTRATOR_TESTDATA_DIR) / relativePath;
    }
    return {};
}

} // namespace

namespace orchestrator
{

std::string readFixture(FixtureRoot root, std::string_view relativePath)
{
    auto const path = fixturePath(root, relativePath);
    auto file = std::ifstream(path, std::ios::binary);
    if (!file)
        FAIL("cannot open fixture " << path.string());
    return { std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
}

std::vector<std::string> fixtureNames(FixtureRoot root, std::string_view relativeDirectory)
{
    auto const path = fixturePath(root, relativeDirectory);
    auto error = std::error_code {};
    auto names = std::vector<std::string> {};
    for (auto const& entry: std::filesystem::directory_iterator(path, error))
        if (entry.is_regular_file())
            names.push_back(entry.path().filename().string());
    if (error)
        FAIL("cannot list fixture directory " << path.string() << ": " << error.message());
    std::ranges::sort(names);
    return names;
}

} // namespace orchestrator

int main(int argc, char const* argv[])
{
    core::testing::suppressWindowsDialogs();

    int const result = Catch::Session().run(argc, argv);
    return result;
}
