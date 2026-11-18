// SPDX-License-Identifier: BSD-2-Clause
// Copyright © 2025 AdrenoTools contributors
//
// C++ API of the Rust layer (adrenotools-rs). The declarations are generated
// by cxxbridge from rust/src/lib.rs at build time: CMake writes
// ${CMAKE_BINARY_DIR}/rust/ffi.h (declarations) and rust/ffi.cc (the C++
// wrappers around the Rust shims, compiled into adrenotools_cxxbridge), and
// puts that directory on the include path of every target that defines
// ADRENOTOOLS_RUST_ENABLED. Do not hand-write declarations here: the
// signatures must match what cxxbridge emits — namespace ffi, shared structs
// by value / const&, `&str` as rust::Str, `&mut u32` as uint32_t&.
#pragma once

#include "ffi.h"
