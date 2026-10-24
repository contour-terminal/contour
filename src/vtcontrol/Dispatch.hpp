// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <string>
#include <string_view>

#include <vtcontrol/Catalog.hpp>
#include <vtcontrol/ControlApi.hpp>

namespace vtcontrol
{

/// Who is calling.
enum class Caller : uint8_t
{
    Socket = 0, ///< A connection over a socket or pipe.
    Extension,  ///< A WebAssembly extension inside the host.
};

/// How a raw (JSON) call reports its outcome.
using RawCompletion = std::move_only_function<void(std::expected<std::string, ControlError>)>;

/// Serves one call: finds the catalog row, decodes `params`, calls the typed handler and encodes the reply.
///
/// An unknown method is `MethodNotFound`; a `SocketOnly` row called by an extension is `Unsupported`
/// and never reaches the handler, nor does a row whose permission the context's grants lack, which is
/// `Forbidden` ("needs grant 'input'"); params that do not decode are `BadRequest` and never reach it either,
/// nor does a request whose row's `requestGrant` names a permission the grants lack (`tab.create` with a
/// command without `input`), which is `Forbidden` as well.
/// @param api The handler.
/// @param context The call's context.
/// @param caller Who is calling.
/// @param method The method name.
/// @param params The request JSON; empty reads as `{}`.
/// @param done Called once with the encoded reply or the error.
void dispatch(ControlApi& api,
              CallContext& context,
              Caller caller,
              std::string_view method,
              std::string_view params,
              RawCompletion done);

} // namespace vtcontrol
