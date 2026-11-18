cmake_minimum_required(VERSION 3.14)

if(NOT ${CMAKE_ANDROID_ARCH_ABI} STREQUAL arm64-v8a)
	message(FATAL_ERROR "Unsupported target architecture: ${CMAKE_ANDROID_ARCH_ABI}. Please make an issue on the repo!")
endif()

project(adrenotools LANGUAGES CXX C)
# Rust is handled entirely via custom commands (cargo build + cxxbridge codegen),
# not via CMake's built-in Rust language support (which doesn't work well with
# Android NDK cross-compilation).

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
endif()

option(OVERCLOCK_BUILD "Enable aggressive performance flags" OFF)

include(CheckIPOSupported)
check_ipo_supported(RESULT lto_supported OUTPUT lto_error)

set(PERF_FLAGS "-O3" "-ffast-math" "-fomit-frame-pointer" "-funroll-loops" "-fvectorize" "-fslp-vectorize")

set(ARM_TARGET_FLAGS "-march=armv8.2-a+crypto+dotprod+fp16")

if(OVERCLOCK_BUILD)
    add_compile_definitions(OVERCLOCK)
	
    list(APPEND PERF_FLAGS 
        "-funroll-loops"
        "-mllvm" "-unroll-threshold=500"
        "-mllvm" "-vectorize-loops"
    )
	
    message(STATUS "Build: Aggressive Overclock Enabled (Heat Warning!)")
else()
    message(STATUS "Build: Stable Standard Mode")
endif()

set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--undefined-version -Wl,--gc-sections -Wl,-O1")

set(GEN_INSTALL_TARGET OFF CACHE BOOL "")

add_subdirectory(lib/linkernsbypass)
add_subdirectory(lib/ShadowHook/shadowhook/src/main/cpp shadowhook_build)

if(TARGET shadowhook AND NOT TARGET shadowhook::shadowhook)
    add_library(shadowhook::shadowhook ALIAS shadowhook)
endif()

file(WRITE "${CMAKE_BINARY_DIR}/shadowhookConfig.cmake" "include_guard()")
file(WRITE "${CMAKE_BINARY_DIR}/shadowhookConfigVersion.cmake" "set(PACKAGE_VERSION 1.0.0)\nset(PACKAGE_ACCEPTABLE TRUE)")

set(shadowhook_DIR "${CMAKE_BINARY_DIR}" CACHE PATH "Path to shadowhook config" FORCE)

include(FetchContent)
FetchContent_Declare(
  bytehook
  GIT_REPOSITORY https://github.com/bytedance/bhook
  GIT_TAG        main
  SOURCE_SUBDIR  bytehook/src/main/cpp
)
FetchContent_MakeAvailable(bytehook)

# ─── Rust + cxx static library (memory-safe layer) ───────────────────────────
# Builds rust/ with cargo + cxx for the Android target. The cxx bridge
# generates a C++ header from rust/src/lib.rs so C++ can call Rust safely.
# Requires: cargo, rustc, cxxbridge, aarch64-linux-android target.
set(ADRENOTOOLS_RUST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/rust")
set(ADRENOTOOLS_RUST_TARGET "aarch64-linux-android")
set(ADRENOTOOLS_RUST_LIB "${CMAKE_CURRENT_BINARY_DIR}/rust/target/${ADRENOTOOLS_RUST_TARGET}/release/libadrenotools_rs.a")
set(ADRENOTOOLS_RUST_HEADER "${CMAKE_CURRENT_BINARY_DIR}/rust/ffi.h")
set(ADRENOTOOLS_RUST_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/rust/ffi.cc")

# Find Rust toolchain
find_program(CARGO_PROGRAM cargo)
find_program(RUSTC_PROGRAM rustc)
find_program(CXXBRIDGE_PROGRAM cxxbridge)

set(ADRENOTOOLS_RUST_AVAILABLE FALSE)
if(CARGO_PROGRAM AND RUSTC_PROGRAM AND CXXBRIDGE_PROGRAM AND EXISTS "${ADRENOTOOLS_RUST_DIR}/Cargo.toml")
    set(ADRENOTOOLS_RUST_AVAILABLE TRUE)
    message(STATUS "Rust toolchain found: cargo=${CARGO_PROGRAM}, rustc=${RUSTC_PROGRAM}, cxxbridge=${CXXBRIDGE_PROGRAM}")
else()
    message(STATUS "Rust toolchain not found — building C++ only (mailbox and GPU mapping stay in C++)")
endif()

if(ADRENOTOOLS_RUST_AVAILABLE)
    # ── Step 1: Generate the C++ side of the cxx bridge ──
    # cxxbridge writes the declarations (ffi.h) and the definitions of the C++
    # wrappers that call the Rust shims (ffi.cc). Both are needed: without
    # ffi.cc every ffi::… call is an undefined reference at link time. The
    # generated files embed the parts of cxx.h they use, so no cxx.h lookup
    # is necessary. -o avoids shell redirection under VERBATIM.
    set(ADRENOTOOLS_RUST_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/rust/ffi.cc")
    add_custom_command(
        OUTPUT "${ADRENOTOOLS_RUST_HEADER}" "${ADRENOTOOLS_RUST_SOURCE}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/rust"
        COMMAND "${CXXBRIDGE_PROGRAM}" "${ADRENOTOOLS_RUST_DIR}/src/lib.rs" --header -o "${ADRENOTOOLS_RUST_HEADER}"
        COMMAND "${CXXBRIDGE_PROGRAM}" "${ADRENOTOOLS_RUST_DIR}/src/lib.rs" -o "${ADRENOTOOLS_RUST_SOURCE}"
        WORKING_DIRECTORY "${ADRENOTOOLS_RUST_DIR}"
        DEPENDS "${ADRENOTOOLS_RUST_DIR}/src/lib.rs"
        COMMENT "Generating C++ bridge (ffi.h, ffi.cc) from rust/src/lib.rs"
        VERBATIM
    )

    # ── Step 2: Build Rust static library ──
    add_custom_command(
        OUTPUT "${ADRENOTOOLS_RUST_LIB}"
        COMMAND ${CMAKE_COMMAND} -E env "CARGO_TARGET_DIR=${CMAKE_CURRENT_BINARY_DIR}/rust/target"
                    "${CARGO_PROGRAM}" build --release --verbose --manifest-path "${ADRENOTOOLS_RUST_DIR}/Cargo.toml" --target "${ADRENOTOOLS_RUST_TARGET}"
        WORKING_DIRECTORY "${ADRENOTOOLS_RUST_DIR}"
        DEPENDS "${ADRENOTOOLS_RUST_DIR}/Cargo.toml" "${ADRENOTOOLS_RUST_DIR}/src/lib.rs"
        COMMENT "Building Rust static library (adrenotools_rs)"
        VERBATIM
    )
    add_custom_target(adrenotools_rs DEPENDS "${ADRENOTOOLS_RUST_LIB}" "${ADRENOTOOLS_RUST_HEADER}" "${ADRENOTOOLS_RUST_SOURCE}")

    # The generated C++ wrappers, compiled once and linked into every target
    # that calls ffi::… (adrenotools, hook_impl). Carries the include path of
    # the generated header and the Rust staticlib itself as usage
    # requirements, so consumers get "… libadrenotools_cxxbridge.a
    # libadrenotools_rs.a" *after* their own objects — the order both GNU ld
    # and lld resolve (a target_link_options() archive comes before the
    # objects and only works because lld is lenient about it). A full path in
    # target_link_libraries() is not checked for existence at configure time;
    # the add_dependencies() makes cargo run first.
    add_library(adrenotools_cxxbridge STATIC "${ADRENOTOOLS_RUST_SOURCE}")
    target_include_directories(adrenotools_cxxbridge PUBLIC "${CMAKE_CURRENT_BINARY_DIR}/rust")
    target_compile_features(adrenotools_cxxbridge PUBLIC cxx_std_17)
    set_target_properties(adrenotools_cxxbridge PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(adrenotools_cxxbridge PUBLIC "${ADRENOTOOLS_RUST_LIB}")
    add_dependencies(adrenotools_cxxbridge adrenotools_rs)
endif()

# ─── C++ sources ─────────────────────────────────────────────────────────────
# mailbox.cpp is always compiled: it uses #ifdef ADRENOTOOLS_RUST_ENABLED to
# delegate to Rust when available, falling back to C++ libc wrappers otherwise.
set(LIB_SOURCES src/bcenabler.cpp
                src/driver.cpp
                src/mailbox.cpp
                include/adrenotools/bcenabler.h
                include/adrenotools/driver.h
                include/adrenotools/priv.h)

add_library(adrenotools ${LIB_SOURCES})

if(ADRENOTOOLS_RUST_AVAILABLE)
    # adrenotools_cxxbridge brings the generated header's include path and
    # the Rust staticlib (linked after the objects that reference it).
    target_link_libraries(adrenotools adrenotools_cxxbridge)
    target_compile_definitions(adrenotools PRIVATE ADRENOTOOLS_RUST_ENABLED)

    add_dependencies(adrenotools adrenotools_rs)
endif()

target_include_directories(adrenotools PUBLIC include)
target_include_directories(adrenotools PRIVATE .)
target_include_directories(adrenotools PRIVATE 
    lib/ShadowHook/shadowhook/src/main/cpp/include
    ${bytehook_SOURCE_DIR}/bytehook/src/main/cpp/include
)

# Apply optimizations cleanly to the primary target
target_compile_options(adrenotools PRIVATE -Wall -Wextra ${PERF_FLAGS} ${ARM_TARGET_FLAGS})
target_link_libraries(adrenotools android -Wl,--whole-archive linkernsbypass -Wl,--no-whole-archive shadowhook bytehook vulkan)

set_target_properties(adrenotools PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

# Enable LTO cleanly on targets if verified available on the host toolchain
if(lto_supported)
    set_target_properties(adrenotools PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(shadowhook PRIVATE -Wno-error=unsafe-buffer-usage -Wno-unsafe-buffer-usage)
    target_compile_options(adrenotools PRIVATE 
        -Wno-error 
        -Wno-c++98-compat 
        -Wno-c++98-compat-pedantic
        -Wno-sign-conversion
        -Wno-gnu-anonymous-struct
        -Wno-nested-anon-types
    )
endif()

if(TARGET shadowhook)
    target_compile_options(shadowhook PRIVATE 
        -Wno-error=declaration-after-statement
        -Wno-declaration-after-statement
        -Wno-error=unsafe-buffer-usage
        -Wno-unsafe-buffer-usage
		-Wno-error=reserved-identifier
        -Wno-reserved-identifier
		-Wno-unused-macros
        -Wno-error=unused-macros
        ${PERF_FLAGS}
        ${ARM_TARGET_FLAGS}
    )
    
    # Force C99 only for ShadowHook's C files
    set_target_properties(shadowhook PROPERTIES 
        C_STANDARD 99
        C_STANDARD_REQUIRED ON
    )
    
    if(lto_supported)
        set_target_properties(shadowhook PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endif()

if(TARGET bytehook)
    set_target_properties(bytehook PROPERTIES INTERFACE_COMPILE_OPTIONS "")
    
    # Re-apply only what ByteHook needs internally
    target_include_directories(bytehook PRIVATE 
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/ShadowHook/shadowhook/src/main/cpp/include
    )
    
    target_compile_options(bytehook PRIVATE 
        -Wno-everything 
        -Wno-error 
        -Wno-declaration-after-statement
        -Wno-unsafe-buffer-usage
        -Wno-pre-c11-compat
        ${PERF_FLAGS}
        ${ARM_TARGET_FLAGS}
    )
    
    set_target_properties(bytehook PROPERTIES 
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
    )
    
    if(lto_supported)
        set_target_properties(bytehook PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endif()

add_subdirectory(src/hook)

if (${GEN_INSTALL_TARGET})
  configure_file(adrenotools.pc.in adrenotools.pc @ONLY)
  install(FILES ${CMAKE_BINARY_DIR}/adrenotools.pc DESTINATION lib/pkgconfig)
  install(TARGETS adrenotools main_hook file_redirect_hook gsl_alloc_hook hook_impl)
  install(DIRECTORY include/
          DESTINATION include
          FILES_MATCHING PATTERN "*.h*")
endif()
