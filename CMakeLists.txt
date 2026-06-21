cmake_minimum_required(VERSION 3.14...4.3)

if(NOT ${CMAKE_ANDROID_ARCH_ABI} STREQUAL arm64-v8a)
	message(FATAL_ERROR "Unsupported target architecture: ${CMAKE_ANDROID_ARCH_ABI}. Please make an issue on the repo!")
endif()

project(adrenotools LANGUAGES CXX C)

option(OVERCLOCK_BUILD "Enable aggressive performance flags" OFF)

if(OVERCLOCK_BUILD)
    add_compile_definitions(OVERCLOCK)
    message(STATUS "Build: Aggressive Overclock Enabled (Heat Warning!)")
else()
    message(STATUS "Build: Stable Standard Mode")
endif()

set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--undefined-version")

set(GEN_INSTALL_TARGET OFF CACHE BOOL "")

add_subdirectory(lib/linkernsbypass)

set(LIB_SOURCES src/bcenabler.cpp
                src/driver.cpp
                include/adrenotools/bcenabler.h
                include/adrenotools/driver.h
                include/adrenotools/priv.h)

add_library(adrenotools ${LIB_SOURCES})

target_include_directories(adrenotools PUBLIC include)
target_include_directories(adrenotools PRIVATE .)

target_compile_options(adrenotools PRIVATE -Wall -Wextra)
target_link_libraries(adrenotools android -Wl,--whole-archive linkernsbypass -Wl,--no-whole-archive vulkan)

set_target_properties(adrenotools PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(adrenotools PRIVATE 
        -Wno-error 
        -Wno-c++98-compat 
        -Wno-c++98-compat-pedantic
        -Wno-sign-conversion
        -Wno-gnu-anonymous-struct
        -Wno-nested-anon-types
    )
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
