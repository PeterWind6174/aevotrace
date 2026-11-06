cmake_minimum_required(VERSION 3.20)
project(aev_p0_engine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(LLVM REQUIRED CONFIG HINTS /opt/homebrew/opt/llvm/lib/cmake/llvm)
message(STATUS "Aevotrace P0: LLVM ${LLVM_PACKAGE_VERSION} (${LLVM_DIR})")
find_library(LLVM_SHARED_LIBRARY NAMES LLVM-23 LLVM
  HINTS ${LLVM_LIBRARY_DIRS})
if(NOT LLVM_SHARED_LIBRARY)
  message(FATAL_ERROR "libLLVM shared library not found (expected Homebrew LLVM)")
endif()
message(STATUS "Aevotrace P0: libLLVM ${LLVM_SHARED_LIBRARY}")

find_path(CAPSTONE_INCLUDE_DIR capstone/capstone.h
  HINTS /opt/homebrew/opt/capstone/include)
find_library(CAPSTONE_LIBRARY NAMES capstone
  HINTS /opt/homebrew/opt/capstone/lib)
if(NOT CAPSTONE_INCLUDE_DIR OR NOT CAPSTONE_LIBRARY)
  message(FATAL_ERROR "Capstone headers/library not found (expected /opt/homebrew/opt/capstone)")
endif()
add_library(aev_capstone UNKNOWN IMPORTED)
set_target_properties(aev_capstone PROPERTIES
  IMPORTED_LOCATION "${CAPSTONE_LIBRARY}"
  INTERFACE_INCLUDE_DIRECTORIES "${CAPSTONE_INCLUDE_DIR}")
message(STATUS "Aevotrace P0: Capstone library ${CAPSTONE_LIBRARY}")

set(AEV_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)
set(AEV_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${AEV_GEN_DIR})

add_library(aev_translator STATIC
  src/coff.cpp
  src/x64.cpp
  src/translator.cpp
  src/ir_emitter.cpp
  src/program_identity.cpp
  src/sha256.cpp
)
target_include_directories(aev_translator PUBLIC ${AEV_INCLUDE_DIR})
target_link_libraries(aev_translator PUBLIC aev_capstone)

add_executable(aev-translate src/translate_main.cpp)
target_link_libraries(aev-translate PRIVATE aev_translator)

# Shared ORC/JIT core used by both the legacy runner and the callback bridge.
add_library(aev_jit_runtime STATIC src/jit_runtime.cpp)
target_include_directories(aev_jit_runtime PUBLIC
  ${AEV_INCLUDE_DIR}
  ${LLVM_INCLUDE_DIRS}
)
target_link_directories(aev_jit_runtime PUBLIC ${LLVM_LIBRARY_DIRS})
target_link_libraries(aev_jit_runtime PUBLIC
  aev_translator ${LLVM_SHARED_LIBRARY} aev_capstone)

add_executable(aev-runner src/runner.cpp src/aev_mem.c)
target_include_directories(aev-runner PRIVATE
  ${AEV_INCLUDE_DIR}
  ${AEV_GEN_DIR}
)
target_link_libraries(aev-runner PRIVATE aev_jit_runtime)

# Reusable C ABI for the GUI path.  The shared library exports only the
# aev_bridge_* API; all Guest execution still goes through the same AOT/JIT
# core and aev_mem helpers.
add_library(aev_bridge SHARED src/aev_bridge.cpp src/aev_mem.c)
target_include_directories(aev_bridge PRIVATE ${AEV_INCLUDE_DIR})
target_link_libraries(aev_bridge PRIVATE
  aev_jit_runtime ${CMAKE_DL_LIBS})
set_target_properties(aev_bridge PROPERTIES
  BUILD_RPATH "${CMAKE_BINARY_DIR}"
  INSTALL_RPATH "@loader_path")

# Test-only harnesses for the instruction-semantics regressions.  The JIT probe
# uses the same emitter and LLVM ORC path as the runner; the AOT probe loads a
# translated dylib; the flags test can target the host or the AOT dylib helper.
add_executable(aev-jit-probe src/jit_probe.cpp src/aev_mem.c)
target_include_directories(aev-jit-probe PRIVATE
  ${AEV_INCLUDE_DIR}
  ${LLVM_INCLUDE_DIRS}
)
target_link_directories(aev-jit-probe PRIVATE ${LLVM_LIBRARY_DIRS})
target_link_libraries(aev-jit-probe PRIVATE
  aev_translator ${LLVM_SHARED_LIBRARY} aev_capstone)

add_executable(aev-aot-probe src/aot_probe.c)
target_include_directories(aev-aot-probe PRIVATE ${AEV_INCLUDE_DIR})
target_link_libraries(aev-aot-probe PRIVATE ${CMAKE_DL_LIBS})

# Test-only: load an AOT dylib and give its dispatcher a real ORC JIT callback.
# Used for the zero-AOT-block hand-off and for the AOT -> JIT fault path.
add_executable(aev-aot-jit-probe src/aot_jit_probe.cpp src/aev_mem.c)
target_include_directories(aev-aot-jit-probe PRIVATE
  ${AEV_INCLUDE_DIR}
  ${LLVM_INCLUDE_DIRS}
)
target_link_directories(aev-aot-jit-probe PRIVATE ${LLVM_LIBRARY_DIRS})
target_link_libraries(aev-aot-jit-probe PRIVATE
  aev_translator ${LLVM_SHARED_LIBRARY} aev_capstone ${CMAKE_DL_LIBS})

add_executable(aev-emitter-probe src/emitter_lock_probe.cpp)
target_link_libraries(aev-emitter-probe PRIVATE aev_translator)

add_executable(aev-flags-test src/flags_test.c src/aev_mem.c)
target_include_directories(aev-flags-test PRIVATE ${AEV_INCLUDE_DIR})
target_link_libraries(aev-flags-test PRIVATE ${CMAKE_DL_LIBS})

set(AEV_INPUT_OBJ ${CMAKE_CURRENT_SOURCE_DIR}/testdata/guest_x64.obj)
set(AEV_LL ${AEV_GEN_DIR}/guest_aot.ll)
set(AEV_HEADER ${AEV_GEN_DIR}/guest_layout.h)
set(AEV_MANIFEST ${AEV_GEN_DIR}/guest_manifest.json)

add_custom_command(
  OUTPUT ${AEV_LL} ${AEV_HEADER} ${AEV_MANIFEST}
  COMMAND aev-translate
          --input ${AEV_INPUT_OBJ}
          --function guest_kernel
          --function guest_op
          --entry guest_kernel
          --out-ll ${AEV_LL}
          --out-header ${AEV_HEADER}
          --out-manifest ${AEV_MANIFEST}
  DEPENDS aev-translate ${AEV_INPUT_OBJ}
  COMMENT "Translating x86-64 COFF .text to LLVM IR and ARM64 layout header"
  VERBATIM
)
add_custom_target(aev_generated DEPENDS ${AEV_LL} ${AEV_HEADER} ${AEV_MANIFEST})
add_dependencies(aev-runner aev_generated)

set(AEV_AOT_OBJ ${AEV_GEN_DIR}/guest_aot.o)
set(AEV_AOT_DYLIB ${AEV_GEN_DIR}/libguest_aot.dylib)
set(AEV_RT_OBJ ${AEV_GEN_DIR}/aot_runtime.o)
set(AEV_MEM_AOT_OBJ ${AEV_GEN_DIR}/aev_mem_aot.o)

add_custom_command(
  OUTPUT ${AEV_AOT_OBJ}
  COMMAND clang --target=arm64-apple-macos14.0 -O2 -c ${AEV_LL} -o ${AEV_AOT_OBJ}
  DEPENDS ${AEV_LL}
  COMMENT "Compiling generated LLVM IR to ARM64 Mach-O object"
  VERBATIM
)

add_custom_command(
  OUTPUT ${AEV_RT_OBJ} ${AEV_MEM_AOT_OBJ}
  COMMAND clang -arch arm64 -O2 -I${AEV_INCLUDE_DIR} -c ${CMAKE_CURRENT_SOURCE_DIR}/src/aot_runtime.c -o ${AEV_RT_OBJ}
  COMMAND clang -arch arm64 -O2 -I${AEV_INCLUDE_DIR} -c ${CMAKE_CURRENT_SOURCE_DIR}/src/aev_mem.c -o ${AEV_MEM_AOT_OBJ}
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/aot_runtime.c ${CMAKE_CURRENT_SOURCE_DIR}/src/aev_mem.c ${AEV_INCLUDE_DIR}/aev_guest.h
  COMMENT "Compiling AOT runtime helpers for ARM64"
  VERBATIM
)

add_custom_command(
  OUTPUT ${AEV_AOT_DYLIB}
  COMMAND clang -arch arm64 -dynamiclib -o ${AEV_AOT_DYLIB}
          ${AEV_AOT_OBJ} ${AEV_RT_OBJ} ${AEV_MEM_AOT_OBJ}
  DEPENDS ${AEV_AOT_OBJ} ${AEV_RT_OBJ} ${AEV_MEM_AOT_OBJ}
  COMMENT "Linking the independently loadable ARM64 AOT dylib"
  VERBATIM
)

add_custom_target(aev_aot ALL DEPENDS ${AEV_AOT_DYLIB})

# Callback fixture: same translator/AOT runtime, fixed synthetic entry
# 0x140001000.  The dylib exports the normal AOT table plus the ABI/input
# identity symbols checked by aev_bridge_create.
set(AEV_CALLBACK_INPUT_OBJ ${CMAKE_CURRENT_SOURCE_DIR}/testdata/callback/aev_callback_fixture.obj)
set(AEV_CALLBACK_LL ${AEV_GEN_DIR}/callback_aot.ll)
set(AEV_CALLBACK_HEADER ${AEV_GEN_DIR}/callback_layout.h)
set(AEV_CALLBACK_MANIFEST ${AEV_GEN_DIR}/callback_manifest.json)
set(AEV_CALLBACK_AOT_OBJ ${AEV_GEN_DIR}/callback_aot.o)
set(AEV_CALLBACK_DYLIB ${AEV_GEN_DIR}/libcallback_aot.dylib)

add_custom_command(
  OUTPUT ${AEV_CALLBACK_LL} ${AEV_CALLBACK_HEADER} ${AEV_CALLBACK_MANIFEST}
  COMMAND aev-translate
          --input ${AEV_CALLBACK_INPUT_OBJ}
          --function aev_guest_callback
          --entry aev_guest_callback
          --out-ll ${AEV_CALLBACK_LL}
          --out-header ${AEV_CALLBACK_HEADER}
          --out-manifest ${AEV_CALLBACK_MANIFEST}
  DEPENDS aev-translate ${AEV_CALLBACK_INPUT_OBJ}
  COMMENT "Translating callback fixture to LLVM IR and ARM64 layout header"
  VERBATIM
)
add_custom_command(
  OUTPUT ${AEV_CALLBACK_AOT_OBJ}
  COMMAND clang --target=arm64-apple-macos14.0 -O2 -c ${AEV_CALLBACK_LL} -o ${AEV_CALLBACK_AOT_OBJ}
  DEPENDS ${AEV_CALLBACK_LL}
  COMMENT "Compiling callback AOT LLVM IR to ARM64 Mach-O object"
  VERBATIM
)
add_custom_command(
  OUTPUT ${AEV_CALLBACK_DYLIB}
  COMMAND clang -arch arm64 -dynamiclib -o ${AEV_CALLBACK_DYLIB}
          ${AEV_CALLBACK_AOT_OBJ} ${AEV_RT_OBJ} ${AEV_MEM_AOT_OBJ}
  DEPENDS ${AEV_CALLBACK_AOT_OBJ} ${AEV_RT_OBJ} ${AEV_MEM_AOT_OBJ}
  COMMENT "Linking the callback ARM64 AOT dylib"
  VERBATIM
)
add_custom_target(aev_callback_aot ALL DEPENDS ${AEV_CALLBACK_DYLIB})

# Mutated input identity negative test: same entry/block addresses, one
# immediate byte changed.  Its AOT dylib must be rejected by input hash.
set(AEV_MUTATED_INPUT_OBJ ${AEV_GEN_DIR}/aev_callback_fixture.mutated.obj)
set(AEV_MUTATED_LL ${AEV_GEN_DIR}/callback_aot_mutated.ll)
set(AEV_MUTATED_HEADER ${AEV_GEN_DIR}/callback_layout_mutated.h)
set(AEV_MUTATED_MANIFEST ${AEV_GEN_DIR}/callback_manifest_mutated.json)
set(AEV_MUTATED_AOT_OBJ ${AEV_GEN_DIR}/callback_aot_mutated.o)
set(AEV_MUTATED_DYLIB ${AEV_GEN_DIR}/libcallback_aot_mutated.dylib)
add_custom_command(
  OUTPUT ${AEV_MUTATED_INPUT_OBJ}
  COMMAND ${CMAKE_COMMAND} -E env python3
          ${CMAKE_CURRENT_SOURCE_DIR}/scripts/mutate_callback_fixture.py
          ${AEV_CALLBACK_INPUT_OBJ} ${AEV_MUTATED_INPUT_OBJ}
  DEPENDS ${AEV_CALLBACK_INPUT_OBJ} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/mutate_callback_fixture.py
  COMMENT "Creating mutated callback object for AOT identity negative test"
  VERBATIM
)
add_custom_command(
  OUTPUT ${AEV_MUTATED_LL} ${AEV_MUTATED_HEADER} ${AEV_MUTATED_MANIFEST}
  COMMAND aev-translate
          --input ${AEV_MUTATED_INPUT_OBJ}
          --function aev_guest_callback
          --entry aev_guest_callback
          --out-ll ${AEV_MUTATED_LL}
          --out-header ${AEV_MUTATED_HEADER}
          --out-manifest ${AEV_MUTATED_MANIFEST}
  DEPENDS aev-translate ${AEV_MUTATED_INPUT_OBJ}
  COMMENT "Translating mutated callback fixture"
  VERBATIM
)
add_custom_command(
  OUTPUT ${AEV_MUTATED_AOT_OBJ}
  COMMAND clang --target=arm64-apple-macos14.0 -O2 -c ${AEV_MUTATED_LL} -o ${AEV_MUTATED_AOT_OBJ}
  DEPENDS ${AEV_MUTATED_LL}
  COMMENT "Compiling mutated callback AOT object"
  VERBATIM
)
add_custom_command(
  OUTPUT ${AEV_MUTATED_DYLIB}
  COMMAND clang -arch arm64 -dynamiclib -o ${AEV_MUTATED_DYLIB}
          ${AEV_MUTATED_AOT_OBJ} ${AEV_RT_OBJ} ${AEV_MEM_AOT_OBJ}
  DEPENDS ${AEV_MUTATED_AOT_OBJ} ${AEV_RT_OBJ} ${AEV_MEM_AOT_OBJ}
  COMMENT "Linking the mutated callback AOT dylib"
  VERBATIM
)
add_custom_target(aev_callback_mutated ALL DEPENDS ${AEV_MUTATED_DYLIB})

# Trace fixture base AOT: default coverage excludes the B-only IMUL block.
set(AEV_TRACE_INPUT_OBJ ${CMAKE_CURRENT_SOURCE_DIR}/testdata/trace/aev_trace_fixture.obj)
set(AEV_TRACE_LL ${AEV_GEN_DIR}/trace_aot.ll)
set(AEV_TRACE_HEADER ${AEV_GEN_DIR}/trace_layout.h)
set(AEV_TRACE_MANIFEST ${AEV_GEN_DIR}/trace_manifest.json)
set(AEV_TRACE_AOT_OBJ ${AEV_GEN_DIR}/trace_aot.o)
set(AEV_TRACE_DYLIB ${AEV_GEN_DIR}/libtrace_aot.dylib)
add_custom_command(
  OUTPUT ${AEV_TRACE_LL} ${AEV_TRACE_HEADER} ${AEV_TRACE_MANIFEST}
  COMMAND aev-translate
          --input ${AEV_TRACE_INPUT_OBJ}
          --function aev_trace_guest
          --entry aev_trace_guest
          --out-ll ${AEV_TRACE_LL}
          --out-header ${AEV_TRACE_HEADER}
          --out-manifest ${AEV_TRACE_MANIFEST}
  DEPENDS aev-translate ${AEV_TRACE_INPUT_OBJ}
  COMMENT "Translating trace fixture to LLVM IR and ARM64 layout header"
  VERBATIM
)
add_custom_command(
  OUTPUT ${AEV_TRACE_AOT_OBJ}
  COMMAND clang --target=arm64-apple-macos14.0 -O2 -c ${AEV_TRACE_LL} -o ${AEV_TRACE_AOT_OBJ}
  DEPENDS ${AEV_TRACE_LL}
  COMMENT "Compiling trace fixture AOT IR to ARM64 object"
  VERBATIM
)
add_custom_command(
  OUTPUT ${AEV_TRACE_DYLIB}
  COMMAND clang -arch arm64 -dynamiclib -o ${AEV_TRACE_DYLIB}
          ${AEV_TRACE_AOT_OBJ} ${AEV_RT_OBJ} ${AEV_MEM_AOT_OBJ}
  DEPENDS ${AEV_TRACE_AOT_OBJ} ${AEV_RT_OBJ} ${AEV_MEM_AOT_OBJ}
  COMMENT "Linking the trace fixture ARM64 AOT dylib"
  VERBATIM
)
add_custom_target(aev_trace_aot ALL DEPENDS ${AEV_TRACE_DYLIB})

enable_testing()
add_test(NAME p0_engine_closed_loop
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_tests.sh
          ${CMAKE_CURRENT_BINARY_DIR}
          ${CMAKE_CURRENT_SOURCE_DIR}
)
set_tests_properties(p0_engine_closed_loop PROPERTIES
  ENVIRONMENT "AEV_AOT_DYLIB=${AEV_AOT_DYLIB};AEV_INPUT_OBJ=${AEV_INPUT_OBJ}"
)

add_test(NAME p0_core_fix_semantics
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_semantics_tests.sh
          ${CMAKE_CURRENT_BINARY_DIR}
          ${CMAKE_CURRENT_SOURCE_DIR}
)
set_tests_properties(p0_core_fix_semantics PROPERTIES
  ENVIRONMENT "AEV_AOT_DYLIB=${AEV_AOT_DYLIB};AEV_INPUT_OBJ=${AEV_INPUT_OBJ}"
)

add_test(NAME p0_engine_empty_aot
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_empty_aot_test.sh
          ${CMAKE_CURRENT_BINARY_DIR}
          ${CMAKE_CURRENT_SOURCE_DIR}
)

add_executable(aev_bridge_test src/aev_bridge_test.cpp)
target_include_directories(aev_bridge_test PRIVATE ${AEV_INCLUDE_DIR})
target_link_libraries(aev_bridge_test PRIVATE aev_bridge)
set_target_properties(aev_bridge_test PROPERTIES
  BUILD_RPATH "${CMAKE_BINARY_DIR}")
add_dependencies(aev_bridge_test aev_callback_aot aev_callback_mutated)

add_test(NAME p0_callback_bridge
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_callback_bridge_test.sh
          ${CMAKE_CURRENT_BINARY_DIR}
          ${CMAKE_CURRENT_SOURCE_DIR}
)
set_tests_properties(p0_callback_bridge PROPERTIES
  ENVIRONMENT "DYLD_LIBRARY_PATH=${CMAKE_BINARY_DIR}")

add_test(NAME p0_trace_aot
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_trace_aot_test.sh
          ${CMAKE_CURRENT_BINARY_DIR}
          ${CMAKE_CURRENT_SOURCE_DIR}
)
set_tests_properties(p0_trace_aot PROPERTIES
  ENVIRONMENT "DYLD_LIBRARY_PATH=${CMAKE_BINARY_DIR}")
