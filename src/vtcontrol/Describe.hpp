// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>
#include <string_view>

#include <vtcontrol/Api.hpp>
#include <vtcontrol/ControlError.hpp>

namespace vtcontrol
{

/// @return Every method and event of the API, with its summary.
[[nodiscard]] ApiListReply listApi();

/// Describes a method or an event; the schemas are generated from the structs themselves.
///
/// For an event, `inputSchema` is `{}` and `outputSchema` is the payload's schema.
/// @param name A method or event name.
/// @return Its description, or `MethodNotFound`.
[[nodiscard]] std::expected<ApiDescription, ControlError> describeApi(std::string_view name);

} // namespace vtcontrol
