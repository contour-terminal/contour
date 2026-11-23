// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <string_view>

#include <orchestrator/core/HostPaths.hpp>

using namespace std::string_view_literals;
using orchestrator::PathStyle;

TEST_CASE("HostPaths: style, join, last component", "[orchestrator][paths]")
{
    CHECK(orchestrator::pathStyleOf(R"(C:\wt)") == PathStyle::Windows);
    CHECK(orchestrator::pathStyleOf(R"(\\s\sh)") == PathStyle::Windows);
    CHECK(orchestrator::pathStyleOf("/wt") == PathStyle::Posix);
    CHECK(orchestrator::pathStyleOf("wt") == std::nullopt);
    CHECK(orchestrator::pathStyleOf("C:wt") == std::nullopt);
    CHECK(orchestrator::pathStyleOf("") == std::nullopt);

    CHECK(orchestrator::joinPath(R"(C:\wt)", "contour", PathStyle::Windows) == R"(C:\wt\contour)");
    CHECK(orchestrator::joinPath(R"(C:\)", "wt", PathStyle::Windows) == R"(C:\wt)");
    CHECK(orchestrator::joinPath("/wt", "contour", PathStyle::Posix) == "/wt/contour");

    CHECK(orchestrator::lastComponent(R"(C:\src\contour)", PathStyle::Windows) == "contour");
    CHECK(orchestrator::lastComponent(R"(\\s\sh\contour)", PathStyle::Windows) == "contour");
    CHECK(orchestrator::lastComponent("/src/contour", PathStyle::Posix) == "contour");
}

TEST_CASE("HostPaths: containment and equality", "[orchestrator][paths]")
{
    CHECK(orchestrator::isStrictlyInside("C:/Users/U/wt/x", R"(C:\Users\u\wt)", PathStyle::Windows));
    CHECK(orchestrator::isStrictlyInside(R"(C:\WT\x)", R"(C:\wt)", PathStyle::Windows));
    CHECK(orchestrator::isStrictlyInside(R"(C:\wt)", R"(C:\)", PathStyle::Windows));
    CHECK_FALSE(orchestrator::isStrictlyInside(R"(C:\wtx)", R"(C:\wt)", PathStyle::Windows));
    CHECK_FALSE(orchestrator::isStrictlyInside(R"(C:\wt)", R"(C:\wt)", PathStyle::Windows));
    CHECK_FALSE(orchestrator::isStrictlyInside(R"(D:\wt\x)", R"(C:\wt)", PathStyle::Windows));
    CHECK_FALSE(orchestrator::isStrictlyInside(R"(\\s\b\x)", R"(\\s\a)", PathStyle::Windows));
    // Only ASCII letters fold: Ä and ä are different names.
    CHECK_FALSE(orchestrator::isStrictlyInside("C:\\\xc3\x84\\x", "C:\\\xc3\xa4", PathStyle::Windows));
    CHECK(orchestrator::isStrictlyInside("/wt/x", "/wt", PathStyle::Posix));
    CHECK_FALSE(orchestrator::isStrictlyInside("/WT/x", "/wt", PathStyle::Posix));
    CHECK_FALSE(orchestrator::isStrictlyInside("/wtx", "/wt", PathStyle::Posix));
    // A backslash is a name byte on POSIX, not a separator.
    CHECK_FALSE(orchestrator::isStrictlyInside(R"(/wt\x)", "/wt", PathStyle::Posix));

    CHECK(orchestrator::sameHostPath(R"(c:\USERS\u)", "C:/Users/u", PathStyle::Windows));
    CHECK_FALSE(orchestrator::sameHostPath(R"(C:\Users\u)", R"(C:\Users\v)", PathStyle::Windows));
    CHECK_FALSE(orchestrator::sameHostPath("/Users/u", "/users/u", PathStyle::Posix));
    CHECK(orchestrator::sameHostPath("/users/u", "/users/u", PathStyle::Posix));

    CHECK(orchestrator::afterHostPath(
              "C:/Users/u/src/contour/.git\n", R"(C:\Users\u\src\contour)", PathStyle::Windows)
          == "/.git\n"sv);
    CHECK(orchestrator::afterHostPath(
              "C:/Users/u/src/other/.git", R"(C:\Users\u\src\contour)", PathStyle::Windows)
          == std::nullopt);
    CHECK(orchestrator::afterHostPath("/src/contour/.git", "/src/contour", PathStyle::Posix) == "/.git"sv);
    CHECK(orchestrator::afterHostPath("/src", "/src/contour", PathStyle::Posix) == std::nullopt);
}

TEST_CASE("HostPaths: a file-read path is always /-separated", "[orchestrator][paths]")
{
    CHECK(orchestrator::slashRelative(R"(C:\wt)", R"(C:\wt\contour\r1\STATUS.md)", PathStyle::Windows)
          == std::optional<std::string> { "contour/r1/STATUS.md" });
    CHECK(orchestrator::slashRelative(R"(C:\wt)", R"(C:\other\contour\r1\STATUS.md)", PathStyle::Windows)
          == std::nullopt);
    CHECK(orchestrator::slashRelative(R"(C:\wt)", R"(C:\wt)", PathStyle::Windows) == std::nullopt);
    CHECK(orchestrator::slashRelative("/wt", "/wt/contour/r1/STATUS.md", PathStyle::Posix)
          == std::optional<std::string> { "contour/r1/STATUS.md" });
    CHECK(orchestrator::slashRelative("/wt", "/wtx/STATUS.md", PathStyle::Posix) == std::nullopt);
}
