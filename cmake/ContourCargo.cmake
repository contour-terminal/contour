# Builds Rust guest crates of the extensions/ cargo workspace into WebAssembly core modules.
#
# Every crate under extensions/ is a member of one workspace (extensions/Cargo.toml) with a committed
# Cargo.lock, so cargo always runs --locked; with CONTOUR_CARGO_OFFLINE it also runs --offline, for
# packagers who vendor the crates (`cargo vendor`) and build without network access.

# Only the guests need Rust; the host build never does. CONTOUR_WASM_GUESTS tells the rest of the
# build whether guest modules (the bundled extensions and the test guests) are built: it follows
# CONTOUR_EXTENSIONS_BUNDLED. That option needs cargo, rustc and the wasm32-wasip1 standard library,
# and a missing piece fails the configuration here, naming the way out, rather than mid-build or with
# an installation that silently lacks the extensions it was meant to ship.

set(CONTOUR_CARGO_WORKSPACE "${PROJECT_SOURCE_DIR}/extensions")
set(CONTOUR_CARGO_TARGET_DIR "${PROJECT_BINARY_DIR}/extensions/target")
set(CONTOUR_WASM_TARGET "wasm32-wasip1")

set(CONTOUR_WASM_GUESTS OFF)
if(CONTOUR_EXTENSIONS_BUNDLED)
    find_program(CARGO_EXECUTABLE cargo)
    find_program(RUSTC_EXECUTABLE rustc)
    set(_wasm_missing "")
    if(NOT CARGO_EXECUTABLE)
        set(_wasm_missing "cargo")
    elseif(NOT RUSTC_EXECUTABLE)
        set(_wasm_missing "rustc")
    else()
        execute_process(
            COMMAND ${RUSTC_EXECUTABLE} --print target-libdir --target ${CONTOUR_WASM_TARGET}
            OUTPUT_VARIABLE _wasm_libdir
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE _wasm_libdir_result
            ERROR_QUIET
        )
        file(GLOB _wasm_std "${_wasm_libdir}/libstd-*.rlib")
        if(NOT _wasm_libdir_result EQUAL 0 OR NOT _wasm_std)
            set(_wasm_missing "the ${CONTOUR_WASM_TARGET} Rust standard library")
        endif()
    endif()
    if(_wasm_missing)
        message(FATAL_ERROR
            "CONTOUR_EXTENSIONS_BUNDLED is ON, but ${_wasm_missing} was not found, so the bundled "
            "status-line extensions cannot be built. Install a Rust toolchain with the "
            "${CONTOUR_WASM_TARGET} target (e.g. `rustup target add ${CONTOUR_WASM_TARGET}`), or "
            "configure with -DCONTOUR_EXTENSIONS_BUNDLED=OFF to build without them.")
    endif()
    set(CONTOUR_WASM_GUESTS ON)
endif()
message(STATUS "WebAssembly guest modules (cargo, ${CONTOUR_WASM_TARGET}): ${CONTOUR_WASM_GUESTS}")

# contour_add_wasm_module(NAME <name> CRATE <dir> OUTPUT_VAR <var>)
#
# Adds a custom target <name>_wasm that runs
#   cargo build --release --target wasm32-wasip1 -p <name>
# for the workspace member at <dir>, and sets <var> in the caller's scope to the path of the built
# <name>.wasm (with '-' replaced by '_', as cargo names it). Callers check CONTOUR_WASM_GUESTS first;
# calling it without the toolchain is a build-script error.
function(contour_add_wasm_module)
    cmake_parse_arguments(_ARG "" "NAME;CRATE;OUTPUT_VAR" "" ${ARGN})
    foreach(_required IN ITEMS NAME CRATE OUTPUT_VAR)
        if(NOT _ARG_${_required})
            message(FATAL_ERROR "contour_add_wasm_module: ${_required} is required")
        endif()
    endforeach()
    if(NOT CONTOUR_WASM_GUESTS)
        message(FATAL_ERROR "contour_add_wasm_module(${_ARG_NAME}) called while CONTOUR_WASM_GUESTS is OFF")
    endif()

    set(_cargo_flags --release --target ${CONTOUR_WASM_TARGET} --locked)
    if(CONTOUR_CARGO_OFFLINE)
        list(APPEND _cargo_flags --offline)
    endif()

    string(REPLACE "-" "_" _artifact "${_ARG_NAME}")
    set(_output "${CONTOUR_CARGO_TARGET_DIR}/${CONTOUR_WASM_TARGET}/release/${_artifact}.wasm")
    # The WIT packages under wit/ are inputs too: guests generate their bindings from them.
    file(GLOB_RECURSE _sources CONFIGURE_DEPENDS
         "${_ARG_CRATE}/src/*.rs" "${_ARG_CRATE}/*.wit" "${PROJECT_SOURCE_DIR}/wit/*.wit")

    add_custom_command(
        OUTPUT "${_output}"
        COMMAND ${CMAKE_COMMAND} -E env "CARGO_TARGET_DIR=${CONTOUR_CARGO_TARGET_DIR}"
                ${CARGO_EXECUTABLE} build ${_cargo_flags}
                --manifest-path "${CONTOUR_CARGO_WORKSPACE}/Cargo.toml" -p ${_ARG_NAME}
        DEPENDS ${_sources}
                "${_ARG_CRATE}/Cargo.toml"
                "${CONTOUR_CARGO_WORKSPACE}/Cargo.toml"
                "${CONTOUR_CARGO_WORKSPACE}/Cargo.lock"
        WORKING_DIRECTORY "${CONTOUR_CARGO_WORKSPACE}"
        COMMENT "Building WebAssembly module ${_ARG_NAME} (cargo, ${CONTOUR_WASM_TARGET})"
        VERBATIM
        USES_TERMINAL
    )
    add_custom_target(${_ARG_NAME}_wasm DEPENDS "${_output}")
    set(${_ARG_OUTPUT_VAR} "${_output}" PARENT_SCOPE)
endfunction()
