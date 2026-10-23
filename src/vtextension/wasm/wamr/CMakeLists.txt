# vtextension_wamr: the WAMR backend of the WebAssembly runtime interface (../WasmRuntime.hpp). The only
# target that links WAMR. vtextension itself does not: whoever composes a program links vtextension_runtime,
# which names the selected backend, and calls makeDefaultBackend() (../DefaultBackend.hpp).
# scripts/check-wasm-runtime-containment.py keeps it that way.

add_library(vtextension_wamr STATIC
    DefaultBackend.cpp
    WamrBackend.cpp
    WamrInstance.cpp
    WamrModule.cpp
    WamrThreadEnv.cpp
    WamrBackend.hpp
    WamrHandles.hpp
    WamrRuntime.hpp
    WamrThreadEnv.hpp
)
target_link_libraries(vtextension_wamr PUBLIC vtextension PRIVATE contour::wamr)

if(TARGET vtextension_test)
    # The backend's own tests drive WAMR's C API directly, so they, and only they, need WAMR's headers.
    # PreparedGeneration.cpp defines the probe runtime-agnostic tests read (../testing/PreparedGeneration.hpp).
    target_sources(vtextension_test PRIVATE PreparedGeneration.cpp WamrBackend_test.cpp)
    target_link_libraries(vtextension_test PRIVATE contour::wamr)
endif()
