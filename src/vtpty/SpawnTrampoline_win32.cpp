// SPDX-License-Identifier: Apache-2.0
#include <vtpty/SpawnTrampoline.hpp>

namespace vtpty
{

// Windows creates processes without fork(), so it needs no trampoline: no invocation is one.

void runSpawnTrampolineIfRequested(int /*argc*/, char const* const* /*argv*/) noexcept
{
}

void runSpawnTrampolineIfRequested(int /*argc*/, char* const* /*argv*/) noexcept
{
}

} // namespace vtpty
