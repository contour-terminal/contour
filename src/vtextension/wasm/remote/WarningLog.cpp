// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/remote/WarningLog.hpp>

namespace vtextension::wasm::remote
{

core::log::Category const warningLog { "vtextension.warning",
                                       "Warns of extensions running, but not quite as they should.",
                                       core::log::Category::State::Enabled };

} // namespace vtextension::wasm::remote
