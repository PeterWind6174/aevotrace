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
)
target_include_directories(aev_translator PUBLIC ${AEV_INCLUDE_DIR})
target_link_libraries(aev_translator PUBLIC aev_capstone)

add_executable(aev-translate src/translate_main.cpp)
target_link_libraries(aev-translate PRIVATE aev_translator)

add_executable(aev-runner src/runner.cpp src/aev_mem.c)
target_include_directories(aev-runner PRIVATE
  ${AEV_INCLUDE_DIR}
  ${LLVM_INCLUDE_DIRS}
  ${AEV_GEN_DIR}
)
target_link_directories(aev-runner PRIVATE ${LLVM_LIBRARY_DIRS})
target_link_libraries(aev-runner PRIVATE aev_translator ${LLVM_SHARED_LIBRARY})
target_link_libraries(aev-runner PRIVATE aev_capstone)

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
