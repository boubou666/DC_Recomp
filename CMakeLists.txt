cmake_minimum_required(VERSION 3.21)
project(DC_Recomp LANGUAGES C CXX)

# Dark Cloud (NTSC-U, SCUS-97111) static recompilation on top of PS2Recomp.
#
# Build the tools first (scripts/build_tools.sh), generate code (scripts/recomp.sh),
# then configure this project to get the `ps2EntryRunner` executable with the
# recompiled game linked in.

set(DC_GENERATED_DIR "${CMAKE_SOURCE_DIR}/generated" CACHE PATH
    "Directory containing the output of ps2_recomp for the game ELF")

# Only the runtime is needed here; the recompiler/analyzer are built separately.
set(PS2X_BUILD_RECOMP OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_ANALYZER OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(PS2X_BUILD_STUDIO OFF CACHE BOOL "" FORCE)
# A total recompilation: the native GS renderer draws everything, paraLLEl-GS (GS emulation) is not even built in.
set(PS2X_ENABLE_PARALLEL_GS OFF CACHE BOOL "" FORCE)

add_subdirectory(PS2Recomp)

file(GLOB DC_GENERATED_SOURCES CONFIGURE_DEPENDS "${DC_GENERATED_DIR}/*.cpp")
if(NOT DC_GENERATED_SOURCES)
    message(FATAL_ERROR
        "No generated sources in ${DC_GENERATED_DIR}. Run scripts/recomp.sh first.")
endif()

# The runtime ships a placeholder register_functions.cpp that defines the same
# function-table globals as the generated one; drop it from the runner.
get_target_property(_runner_sources ps2EntryRunner SOURCES)
list(FILTER _runner_sources EXCLUDE REGEX "src/runner/register_functions\.cpp$")
set_property(TARGET ps2EntryRunner PROPERTY SOURCES ${_runner_sources})

file(GLOB DC_RUNTIME_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/runtime/*.cpp")
# Out of the unity batches: an edit to the game layer then rebuilds one small file, not a batch of generated code.
set_source_files_properties(${DC_RUNTIME_SOURCES} TARGET_DIRECTORY ps2EntryRunner PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)

# IOP modules recompiled to native C++ by the fork's tools/iop_recomp.py (scripts/iop_recomp.sh, from the modules
# PS2X_IOP_DUMP_MODULES saved; PS2X_IOP_NATIVE=1 runs them, as in TMB): their own object library, outside the
# unity batches (each file's names are its own), without debug info.
file(GLOB DC_IOP_NATIVE_SOURCES CONFIGURE_DEPENDS "${DC_GENERATED_DIR}/iop/*.cpp")
if(DC_IOP_NATIVE_SOURCES)
    add_library(dc_iop_native OBJECT ${DC_IOP_NATIVE_SOURCES})
    target_link_libraries(dc_iop_native PRIVATE ps2x::iop)
    set_target_properties(dc_iop_native PROPERTIES UNITY_BUILD OFF)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(dc_iop_native PRIVATE -O2 -g0)
    endif()
    target_sources(ps2EntryRunner PRIVATE $<TARGET_OBJECTS:dc_iop_native>)
endif()

# Code overlays recompiled as overlay modules by scripts/overlays.sh (generated/overlays/<module>/): their own object
# library in unity batches (every name carries its module prefix), without debug info.
file(GLOB_RECURSE DC_OVERLAY_SOURCES CONFIGURE_DEPENDS "${DC_GENERATED_DIR}/overlays/*.cpp")
if(DC_OVERLAY_SOURCES)
    add_library(dc_overlays OBJECT ${DC_OVERLAY_SOURCES})
    target_link_libraries(dc_overlays PRIVATE ps2_runtime)
    # Each module's files include its <module>_stubs.h / _functions.h by name: scripts/overlays.sh copies them all
    # into one folder (one include directory per module overflowed the Windows command line).
    target_include_directories(dc_overlays PRIVATE "${DC_GENERATED_DIR}/overlays/include" "${DC_GENERATED_DIR}")
    set_target_properties(dc_overlays PROPERTIES UNITY_BUILD ON UNITY_BUILD_BATCH_SIZE 32)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(dc_overlays PRIVATE -O2 -g0)
    endif()
    target_sources(ps2EntryRunner PRIVATE $<TARGET_OBJECTS:dc_overlays>)
endif()

# VU1 microcode compiled ahead of time by ps2_vu_aot (scripts/recomp.sh): an object library, built at -O3 outside
# the unity batches. Each file takes GCC several GB: at most 4 compile at once.
file(GLOB DC_VU1_AOT_SOURCES CONFIGURE_DEPENDS "${DC_GENERATED_DIR}/vu1/*.cpp")
if(DC_VU1_AOT_SOURCES)
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS dc_vu1_aot=4)
    add_library(dc_vu1_aot OBJECT ${DC_VU1_AOT_SOURCES})
    target_link_libraries(dc_vu1_aot PRIVATE ps2_runtime)
    set_target_properties(dc_vu1_aot PROPERTIES UNITY_BUILD OFF JOB_POOL_COMPILE dc_vu1_aot)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(dc_vu1_aot PRIVATE -O3 -g0)
    endif()
    target_sources(ps2EntryRunner PRIVATE $<TARGET_OBJECTS:dc_vu1_aot>)
endif()

# register_functions.cpp fills the function table in one long constructor (JC2: ~214,000 stores, 35 min with GCC at
# -O2 -g), the tail of every full build. It runs once at startup, so build it alone and unoptimized.
set_source_files_properties("${DC_GENERATED_DIR}/register_functions.cpp" TARGET_DIRECTORY ps2EntryRunner PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties("${DC_GENERATED_DIR}/register_functions.cpp" TARGET_DIRECTORY ps2EntryRunner PROPERTIES COMPILE_OPTIONS "-O0;-g0")
endif()

target_sources(ps2EntryRunner PRIVATE ${DC_GENERATED_SOURCES} ${DC_RUNTIME_SOURCES})
target_include_directories(ps2EntryRunner PRIVATE "${DC_GENERATED_DIR}")

# Line tables only for the runner's own sources (the generated code): full -g makes 2.8 GB of DWARF, which slows every
# batch, the link and the split. Backtraces through generated code keep file:line; the runtime library keeps full -g.
# (A per-source option would take the files out of their unity batches, hence the target option, after the config's -g.)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(ps2EntryRunner PRIVATE $<$<CONFIG:RelWithDebInfo,Debug>:-g1>)
endif()

# Windows loads no image over 2 GB, and with debug info the runner is ~3 GB (2.8 GB of DWARF for the generated
# code, 106 MB of .text). Move the debug info to ps2EntryRunner.debug next to it; gdb finds it through the debug link.
# ps2EntryRunner is created in the submodule, where POST_BUILD cannot reach it: `dc_runner` (what
# scripts/build_runtime.sh builds) splits once per relink, tracked by a stamp file.
if(WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND CMAKE_OBJCOPY AND CMAKE_STRIP)
    set(_dc_split_stamp "${CMAKE_BINARY_DIR}/ps2EntryRunner.split.stamp")
    add_custom_command(OUTPUT "${_dc_split_stamp}"
        COMMAND "${CMAKE_OBJCOPY}" --only-keep-debug "$<TARGET_FILE:ps2EntryRunner>" "$<TARGET_FILE_DIR:ps2EntryRunner>/ps2EntryRunner.debug"
        COMMAND "${CMAKE_STRIP}" --strip-debug "$<TARGET_FILE:ps2EntryRunner>"
        COMMAND "${CMAKE_OBJCOPY}" "--add-gnu-debuglink=$<TARGET_FILE_DIR:ps2EntryRunner>/ps2EntryRunner.debug" "$<TARGET_FILE:ps2EntryRunner>"
        COMMAND "${CMAKE_COMMAND}" -E touch "${_dc_split_stamp}"
        DEPENDS ps2EntryRunner
        COMMENT "Moving the runner's debug info to ps2EntryRunner.debug (Windows image size limit)"
        VERBATIM)
    add_custom_target(dc_runner ALL DEPENDS "${_dc_split_stamp}")
else()
    add_custom_target(dc_runner ALL DEPENDS ps2EntryRunner)
endif()

list(LENGTH DC_GENERATED_SOURCES _dc_count)
message(STATUS "DC_Recomp: linking ${_dc_count} generated source files from ${DC_GENERATED_DIR}")
