// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <vtcontrol/EnumTable.hpp>

namespace vtcontrol
{

/// Why a control call failed.
enum class ControlErrorCode : uint8_t
{
    BadRequest = 0, ///< The request is malformed: an unknown key, a wrong type, a bad value.
    MethodNotFound, ///< No such method.
    NotFound,       ///< The target names an object that does not exist.
    Ambiguous,      ///< The target matches more than one object; `candidates` lists them.
    Unsupported,    ///< The method exists but this daemon or object cannot do it.
    Forbidden,      ///< The connection's grants do not cover the method.
    Failed,         ///< Anything else.
    LimitReached,   ///< The connection holds as many subscriptions or waits as it may; end one first.
};

template <>
struct EnumTable<ControlErrorCode>
{
    static constexpr std::array Rows {
        EnumName { ControlErrorCode::BadRequest, "badRequest" },
        EnumName { ControlErrorCode::MethodNotFound, "methodNotFound" },
        EnumName { ControlErrorCode::NotFound, "notFound" },
        EnumName { ControlErrorCode::Ambiguous, "ambiguous" },
        EnumName { ControlErrorCode::Unsupported, "unsupported" },
        EnumName { ControlErrorCode::Forbidden, "forbidden" },
        EnumName { ControlErrorCode::Failed, "failed" },
        EnumName { ControlErrorCode::LimitReached, "limitReached" },
    };
};

/// A failed control call: a code, a message for people, and the candidates of an ambiguous target.
struct ControlError
{
    ControlErrorCode code = ControlErrorCode::Failed; ///< Why the call failed.
    std::string message;                              ///< What went wrong, for a person.
    std::vector<std::string> candidates;              ///< Object ids an `Ambiguous` target matched.
};

} // namespace vtcontrol
