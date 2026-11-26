# vtextension_wamr: the WAMR backend of the WebAssembly runtime interface (../WasmRuntime.hpp). The only
# target that links WAMR. vtextension itself does not: a program running extensions in process links it through
# vtextension_runtime and calls makeDefaultBackend() (../DefaultBackend.hpp), and the tests that run guests in
# process name it.
# scripts/check-wasm-runtime-containment.py keeps it that way.

add_library(vtextension_wamr STATIC
    DefaultBackend.cpp
    HiddenExports.cpp
    WamrGlobals.cpp
    TableCaps.cpp
    WamrBackend.cpp
    WamrInstance.cpp
    WamrModule.cpp
    WamrThreadEnv.cpp
    WasiOverrides.cpp
    HiddenExports.hpp
    WamrGlobals.hpp
    TableCaps.hpp
    WamrBackend.hpp
    WamrHandles.hpp
    WamrRuntime.hpp
    WamrThreadEnv.hpp
    WasiOverrides.hpp
)
target_link_libraries(vtextension_wamr PUBLIC vtextension PRIVATE contour::wamr)

# The engine an extension host process drives (WamrEngine.hpp): the backend, holding the null device and the
# runtime it needs before the host locks itself down.
add_library(vtextension_wamr_engine STATIC
    WamrEngine.cpp
    WamrEngine.hpp
)
target_link_libraries(vtextension_wamr_engine PUBLIC vtextension_wamr vtextension_remote_wire PRIVATE contour::wamr)

# The engine the extension host executable runs (../host/HelperEngine.hpp), where it runs WAMR's
# (CONTOUR_WASM_RUNTIME, ../../../../CMakeLists.txt).
add_library(vtextension_wamr_helper_engine STATIC
    HelperEngine.cpp
    ${PROJECT_SOURCE_DIR}/src/vtextension/wasm/host/HelperEngine.hpp
)
target_link_libraries(vtextension_wamr_helper_engine PUBLIC vtextension_host PRIVATE vtextension_wamr_engine)

if(TARGET vtextension_test)
    # The backend's own tests drive WAMR's C API directly, so they, and only they, need WAMR's headers.
    # PreparedGeneration.cpp defines the probe runtime-agnostic tests read (../testing/PreparedGeneration.hpp).
    target_sources(vtextension_test PRIVATE
        HiddenExports_test.cpp
        PreparedGeneration.cpp
        TableCaps_test.cpp
        WamrBackend_test.cpp
        WamrEngine_test.cpp
    )
    target_link_libraries(vtextension_test PRIVATE vtextension_wamr_engine contour::wamr)
endif()
