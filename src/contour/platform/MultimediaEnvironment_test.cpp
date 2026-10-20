// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/MultimediaEnvironment.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string_view>
#include <vector>

using contour::platform::multimediaProbeVariablesToEmpty;

TEST_CASE("multimediaProbeVariablesToEmpty: no hardware video device is probed for a bell",
          "[contour][platform][multimedia]")
{
    // Qt Multimedia's FFmpeg backend opens every GPU when it loads, for hardware video decoding and
    // encoding Contour never does -- and a GPU whose driver hangs in open() holds the process for as
    // long, at startup and, since the probe is waited for, at exit too.
    auto const nothingSet = [](std::string_view) {
        return false;
    };
    auto const variables = multimediaProbeVariablesToEmpty(nothingSet);

    CHECK(variables
          == std::vector<std::string_view> { "QT_FFMPEG_DECODING_HW_DEVICE_TYPES",
                                             "QT_FFMPEG_ENCODING_HW_DEVICE_TYPES" });
}

TEST_CASE("multimediaProbeVariablesToEmpty: a variable the user exported is theirs",
          "[contour][platform][multimedia]")
{
    auto const decodingSet = [](std::string_view name) {
        return name == "QT_FFMPEG_DECODING_HW_DEVICE_TYPES";
    };
    CHECK(multimediaProbeVariablesToEmpty(decodingSet)
          == std::vector<std::string_view> { "QT_FFMPEG_ENCODING_HW_DEVICE_TYPES" });

    auto const allSet = [](std::string_view) {
        return true;
    };
    CHECK(multimediaProbeVariablesToEmpty(allSet).empty());
}
