// SPDX-License-Identifier: Apache-2.0
//
// Compiled without glaze on the include path (see vtextension_public_headers_test in CMakeLists.txt): every
// header of vtextension but the private ControlCodec.hpp, each of which another module may include, so
// none of them may reach glaze.
#include <vtextension/AtomicSharedPtr.hpp>
#include <vtextension/BundledExtensions.hpp>
#include <vtextension/CanonicalAbi.hpp>
#include <vtextension/CommandLine.hpp>
#include <vtextension/CommandPolicy.hpp>
#include <vtextension/CompiledExtension.hpp>
#include <vtextension/ControlImports.hpp>
#include <vtextension/DeadlineGuard.hpp>
#include <vtextension/Engine.hpp>
#include <vtextension/Event.hpp>
#include <vtextension/EventQueue.hpp>
#include <vtextension/ExtensionComposition.hpp>
#include <vtextension/ExtensionConsent.hpp>
#include <vtextension/ExtensionError.hpp>
#include <vtextension/ExtensionGrantStore.hpp>
#include <vtextension/ExtensionHost.hpp>
#include <vtextension/ExtensionLimits.hpp>
#include <vtextension/ExtensionScheduler.hpp>
#include <vtextension/ExtensionWorkerPool.hpp>
#include <vtextension/HostBindings.hpp>
#include <vtextension/HostState.hpp>
#include <vtextension/IHostServices.hpp>
#include <vtextension/Instance.hpp>
#include <vtextension/Manifest.hpp>
#include <vtextension/Placement.hpp>
#include <vtextension/Runner.hpp>
#include <vtextension/SessionExtensions.hpp>
#include <vtextension/StatusLineAdapter.hpp>
#include <vtextension/StatuslineWorld.hpp>
#include <vtextension/Surface.hpp>
#include <vtextension/Utf8.hpp>
#include <vtextension/wasm/DefaultBackend.hpp>
#include <vtextension/wasm/WasiAllowList.hpp>
#include <vtextension/wasm/WasmRuntime.hpp>
