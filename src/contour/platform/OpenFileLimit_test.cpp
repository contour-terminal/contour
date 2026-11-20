// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/OpenFileLimit.hpp>

#include <QtCore/QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#ifdef _WIN32
    #include <share.h>
#endif

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <ranges>
#include <string>
#include <tuple>
#include <vector>

namespace
{

/// More streams than the Windows C runtime holds open by default (512), with room for what the run
/// already holds.
constexpr auto StreamsPastTheDefault = 600;

/// Closes a C stream.
struct StreamCloser
{
    void operator()(std::FILE* stream) const noexcept { std::ignore = std::fclose(stream); }
};

} // namespace

TEST_CASE("the process can hold more files open than the C runtime's default", "[contour][platform]")
{
    auto const raised = contour::platform::raiseOpenFileLimit();
    INFO((raised ? std::string {} : raised.error()));
    REQUIRE(raised.has_value());

    QTemporaryDir const dir;
    REQUIRE(dir.isValid());
    auto const path = std::filesystem::path(dir.path().toStdU16String()) / "font.ttf";
    std::ofstream(path) << "a face";

    // As FreeType holds font files: each one through its own C stream, all at once.
    auto streams = std::vector<std::unique_ptr<std::FILE, StreamCloser>> {};
    for ([[maybe_unused]] auto const _: std::views::iota(0, StreamsPastTheDefault))
    {
#ifdef _WIN32
        // Shared, as FreeType's fopen opens it: _wfopen is deprecated, _wfopen_s denies a second open.
        auto* stream = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);
#else
        auto* stream = std::fopen(path.c_str(), "rb");
#endif
        REQUIRE(stream != nullptr);
        streams.emplace_back(stream);
    }
    CHECK(streams.size() == StreamsPastTheDefault);
}
