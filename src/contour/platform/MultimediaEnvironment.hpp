// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <functional>
#include <string_view>
#include <vector>

namespace contour::platform
{

/// Variables through which Qt Multimedia's FFmpeg backend is told which hardware video devices to probe
/// when it loads. Set to the empty string, they name none.
///
/// Contour loads Qt Multimedia for the bell's sound alone, and the probe opens every GPU -- through Vulkan,
/// VA-API and the rest -- for decoding and encoding video it never does. It wakes a sleeping discrete GPU,
/// and one whose driver hangs in open() holds the probe for as long: the bell is not ready until then, and
/// since the probe is waited for before Qt is torn down, neither does the process exit.
inline constexpr auto MultimediaHardwareProbeVariables = std::array<std::string_view, 2> {
    "QT_FFMPEG_DECODING_HW_DEVICE_TYPES",
    "QT_FFMPEG_ENCODING_HW_DEVICE_TYPES",
};

/// The variables to set to the empty string so that loading Qt Multimedia probes no hardware video device.
/// Qt reads each once, when the backend loads, so they must be set before that.
/// @param isAlreadySet Whether a variable is already in the environment (the user's choice wins).
/// @return Those of #MultimediaHardwareProbeVariables the user has not set.
[[nodiscard]] inline std::vector<std::string_view> multimediaProbeVariablesToEmpty(
    std::function<bool(std::string_view)> const& isAlreadySet)
{
    auto variables = std::vector<std::string_view> {};
    for (auto const name: MultimediaHardwareProbeVariables)
        if (!isAlreadySet(name))
            variables.push_back(name);
    return variables;
}

} // namespace contour::platform
