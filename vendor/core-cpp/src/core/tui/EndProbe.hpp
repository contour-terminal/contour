// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace core::tui
{

/// @brief What an input source answers when asked, without a read, whether its input has ended.
///
/// Most inputs report their end as readiness: a POSIX terminal that hangs up, or a pipe whose writer
/// closed, makes its handle readable, and the read behind that readiness finds the end. A Windows
/// console whose host process died does not. Its input handle is never signalled again, while every
/// console call on it fails at once with `ERROR_PIPE_NOT_CONNECTED`, so a reader that waits for
/// readiness waits for ever and the process outlives its console. Such an input has to be asked.
enum class EndProbe : std::uint8_t
{
    NotNeeded, ///< The end arrives as readiness, so asking adds nothing; ask no more (the default).
    Open,      ///< The input is still there; ask again later.
    Ended,     ///< The input has ended for good; nothing more will be read from it.
};

} // namespace core::tui
