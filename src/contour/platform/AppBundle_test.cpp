// SPDX-License-Identifier: Apache-2.0
//
// The gate that keeps the bare binary, a test run and `contour` from a shell away from
// `+[UNUserNotificationCenter currentNotificationCenter]`, which raises outside an application bundle.

#include <contour/platform/AppBundle.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <string_view>

using contour::platform::AppBundle;
using contour::platform::appBundleOf;

namespace
{
constexpr auto Identifier = std::string_view { "org.contourterminal.Contour" };

struct BundleCase
{
    std::string_view path;
    std::string_view identifier;
    AppBundle expected;
};
} // namespace

TEST_CASE("appBundleOf: only an .app with an identifier is a bundle", "[appbundle]")
{
    auto const row =
        GENERATE(as<BundleCase> {},
                 BundleCase { "/Applications/Contour.app", Identifier, AppBundle::Present },
                 BundleCase { "/Applications/Contour.app/", Identifier, AppBundle::Present },
                 BundleCase { "/Applications/Contour.app//", Identifier, AppBundle::Present },
                 BundleCase { "/Users/me/out/src/contour", Identifier, AppBundle::Absent },
                 BundleCase { "/Applications/Contour.app", "", AppBundle::Absent },
                 BundleCase { "/Applications/Contour.APP.bak", Identifier, AppBundle::Absent },
                 BundleCase { "/Applications/Contour.APP", Identifier, AppBundle::Absent },
                 BundleCase { "/Applications/Contour.app/Contents/MacOS", Identifier, AppBundle::Absent },
                 BundleCase { "", Identifier, AppBundle::Absent },
                 BundleCase { "/", Identifier, AppBundle::Absent });

    CHECK(appBundleOf(row.path, row.identifier) == row.expected);
}
