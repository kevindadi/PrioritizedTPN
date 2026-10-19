cmake_minimum_required(VERSION 3.27)

# vcpkg toolchain must be set before project().
#
# Auto-detection is restricted to Windows. On Linux/macOS the system Boost and
# CMake packages are used instead, because bootstrapping a vcpkg manifest tree
# there is slow and usually redundant. An explicitly supplied vcpkg toolchain
# (e.g. -DCMAKE_TOOLCHAIN_FILE=.../vcpkg.cmake) is still honored everywhere,
# and -DPTPN_USE_VCPKG=ON re-enables auto-detection on non-Windows hosts.
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
    set(_ptpn_vcpkg_auto_default ON)
else()
    set(_ptpn_vcpkg_auto_default OFF)
endif()
option(PTPN_USE_VCPKG
    "Auto-detect a vcpkg toolchain (default: ON on Windows, OFF elsewhere)"
    ${_ptpn_vcpkg_auto_default})
unset(_ptpn_vcpkg_auto_default)

if(CMAKE_TOOLCHAIN_FILE MATCHES "[/\\\\]vcpkg\\.cmake$")
    message(STATUS "Using vcpkg toolchain: ${CMAKE_TOOLCHAIN_FILE}")
elseif(PTPN_USE_VCPKG)
    foreach(_ptpn_vcpkg_root
        "$ENV{VCPKG_ROOT}"
        "$ENV{VCPKG_INSTALLATION_ROOT}"
        "$ENV{HOME}/vcpkg"
    )
        if(_ptpn_vcpkg_root AND EXISTS "${_ptpn_vcpkg_root}/scripts/buildsystems/vcpkg.cmake")
            set(CMAKE_TOOLCHAIN_FILE "${_ptpn_vcpkg_root}/scripts/buildsystems/vcpkg.cmake"
                CACHE FILEPATH "Vcpkg toolchain file")
            message(STATUS "Using vcpkg from: ${_ptpn_vcpkg_root}")
            break()
        endif()
    endforeach()
    unset(_ptpn_vcpkg_root)
else()
    message(STATUS "vcpkg auto-detection disabled on ${CMAKE_HOST_SYSTEM_NAME} "
                   "(pass -DPTPN_USE_VCPKG=ON to enable)")
endif()

# Pick a host-matching triplet when vcpkg is active and the caller did not set one.
if(CMAKE_TOOLCHAIN_FILE MATCHES "[/\\\\]vcpkg\\.cmake$" AND NOT VCPKG_TARGET_TRIPLET)
    if(DEFINED ENV{VCPKG_DEFAULT_TRIPLET} AND NOT "$ENV{VCPKG_DEFAULT_TRIPLET}" STREQUAL "")
        set(VCPKG_TARGET_TRIPLET "$ENV{VCPKG_DEFAULT_TRIPLET}" CACHE STRING "vcpkg target triplet")
    else()
        # CMAKE_HOST_SYSTEM_PROCESSOR is empty before project() on macOS/Linux.
        set(_ptpn_host_processor "${CMAKE_HOST_SYSTEM_PROCESSOR}")
        if(_ptpn_host_processor STREQUAL "")
            if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
                set(_ptpn_host_processor "$ENV{PROCESSOR_ARCHITECTURE}")
            else()
                execute_process(
                    COMMAND uname -m
                    OUTPUT_VARIABLE _ptpn_host_processor
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET
                )
            endif()
        endif()
        string(TOLOWER "${_ptpn_host_processor}" _ptpn_host_processor)

        if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
            if(_ptpn_host_processor MATCHES "arm64|aarch64")
                set(VCPKG_TARGET_TRIPLET "arm64-osx" CACHE STRING "vcpkg target triplet")
            else()
                set(VCPKG_TARGET_TRIPLET "x64-osx" CACHE STRING "vcpkg target triplet")
            endif()
        elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
            if(_ptpn_host_processor MATCHES "arm64|aarch64")
                set(VCPKG_TARGET_TRIPLET "arm64-linux" CACHE STRING "vcpkg target triplet")
            else()
                set(VCPKG_TARGET_TRIPLET "x64-linux" CACHE STRING "vcpkg target triplet")
            endif()
        elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
            if(_ptpn_host_processor MATCHES "arm64|aarch64")
                set(VCPKG_TARGET_TRIPLET "arm64-windows" CACHE STRING "vcpkg target triplet")
            else()
                set(VCPKG_TARGET_TRIPLET "x64-windows" CACHE STRING "vcpkg target triplet")
            endif()
        endif()
        unset(_ptpn_host_processor)
    endif()
    if(VCPKG_TARGET_TRIPLET)
        message(STATUS "vcpkg target triplet: ${VCPKG_TARGET_TRIPLET}")
    endif()
endif()

project(PTPN LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Suppress warnings from dependencies with older cmake_minimum_required
cmake_policy(SET CMP0057 NEW)

# FetchContent for dependencies
include(FetchContent)
include(CTest)

# nlohmann/json
FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP true
)
FetchContent_MakeAvailable(json)

# CLI11
FetchContent_Declare(cli11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG v2.6.2
)
FetchContent_MakeAvailable(cli11)

# spdlog (FetchContent avoids vcpkg pkg-config dependency on Linux)
FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.15.3
)
FetchContent_MakeAvailable(spdlog)

# fmt (spdlog dependency, also used directly)
FetchContent_Declare(fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG 11.2.0
)
FetchContent_MakeAvailable(fmt)

set(PTPN_BOOST_COMPONENTS graph filesystem thread date_time)

# CONFIG mode (vcpkg / BoostConfig.cmake); fall back to FindBoost on system installs.
find_package(Boost CONFIG QUIET COMPONENTS ${PTPN_BOOST_COMPONENTS})
if(Boost_FOUND)
    message(STATUS "Boost ${Boost_VERSION} (CONFIG)")
else()
    find_package(Boost REQUIRED COMPONENTS ${PTPN_BOOST_COMPONENTS})
    message(STATUS "Boost ${Boost_VERSION} (system paths)")
endif()

# Module libraries. Each target owns one layer of the pipeline and declares
# only the dependencies it actually uses, so build order and layering stay
# explicit. `PTPN_core` below is the aggregate used by the executables.
add_library(ptpn_types STATIC
    src/model/types.cpp
)
target_include_directories(ptpn_types PUBLIC src)

# Structured, versioned diagnostics shared by the parsers, the pipeline and
# the CLI. nlohmann/json is a PUBLIC dependency because the envelope
# serialization is part of this library's API.
add_library(ptpn_diagnostics STATIC
    src/common/diagnostics.cpp
)
target_include_directories(ptpn_diagnostics PUBLIC src)
target_link_libraries(ptpn_diagnostics
    PUBLIC
    nlohmann_json::nlohmann_json
)

# RTSL front-end: lexer, panic-mode parser, semantic validation, and lowering
# to the in-memory TDG. Lowering does not touch the analysis core.
add_library(ptpn_dsl STATIC
    src/dsl/lexer.cpp
    src/dsl/parser.cpp
    src/dsl/validate.cpp
    src/dsl/lower.cpp
)
target_link_libraries(ptpn_dsl
    PUBLIC
    ptpn_diagnostics
    ptpn_types
)

add_library(ptpn_json STATIC
    src/parse/json.cpp
    src/parse/json_writer.cpp
)
target_link_libraries(ptpn_json
    PUBLIC
    ptpn_types
    ptpn_diagnostics
    nlohmann_json::nlohmann_json
    PRIVATE
    spdlog::spdlog
)

add_library(ptpn_tdg STATIC
    src/model/tdg.cpp
)
target_link_libraries(ptpn_tdg
    PUBLIC
    ptpn_types
    PRIVATE
    ptpn_json
    spdlog::spdlog
)

add_library(ptpn_petri STATIC
    src/model/petri.cpp
)
target_link_libraries(ptpn_petri
    PUBLIC
    ptpn_types
    PRIVATE
    spdlog::spdlog
)

add_library(ptpn_export STATIC
    src/export/export_ptpn.cpp
    src/export/export_dot.cpp
    src/export/scg_dot.cpp
)
target_link_libraries(ptpn_export
    PUBLIC
    ptpn_petri
    ptpn_reachability
    PRIVATE
    Boost::filesystem
    spdlog::spdlog
)

add_library(ptpn_lowering STATIC
    src/lower/tdg2pn/tdg2pn.cpp
    src/lower/tdg2pn/vertex_lowering.cpp
    src/lower/tdg2pn/edge_lowering.cpp
    src/lower/tdg2pn/binding_lowering.cpp
    src/lower/tdg2pn/resource_lowering.cpp
    src/lower/tdg2pn/task_metadata.cpp
    src/lower/tdg2pn/preemption.cpp
    src/lower/tdg2pn/policy_priorities.cpp
)
target_link_libraries(ptpn_lowering
    PUBLIC
    ptpn_petri
    ptpn_tdg
    PRIVATE
    spdlog::spdlog
)

add_library(ptpn_parser STATIC
    src/parse/ptpn_parser.cpp
)
target_link_libraries(ptpn_parser
    PUBLIC
    ptpn_petri
    ptpn_diagnostics
)

add_library(ptpn_dbm STATIC
    src/analysis/dbm/dbm.cpp
    src/analysis/dbm/dbm_clocks.cpp
    src/analysis/dbm/dbm_relations.cpp
)
target_link_libraries(ptpn_dbm
    PUBLIC
    ptpn_types
)

add_library(ptpn_reachability STATIC
    src/analysis/reachability/state_class.cpp
    src/analysis/reachability/ptpn_analysis.cpp
    src/analysis/reachability/state_class_layout.cpp
    src/analysis/reachability/state_class_firing.cpp
    src/analysis/reachability/canonicalization.cpp
    src/analysis/reachability/scheduling.cpp
    src/analysis/reachability/scheduling_policy.cpp
    src/analysis/reachability/symmetry.cpp
)
target_link_libraries(ptpn_reachability
    PUBLIC
    ptpn_dbm
    ptpn_petri
    Boost::graph
    PRIVATE
    spdlog::spdlog
)

add_library(ptpn_metrics STATIC
    src/analysis/metrics/metrics.cpp
    src/analysis/metrics/metrics_structural.cpp
    src/analysis/metrics/metrics_timing.cpp
    src/analysis/metrics/metrics_deadlock.cpp
    src/analysis/metrics/metrics_locks.cpp
    src/analysis/metrics/metrics_utilisation.cpp
    src/analysis/metrics/metrics_json.cpp
)
target_link_libraries(ptpn_metrics
    PUBLIC
    ptpn_reachability
)

# Analysis-side structural reductions (per-task cone projection).
add_library(ptpn_reductions STATIC
    src/analysis/reductions/cone_projection.cpp
)
target_link_libraries(ptpn_reductions
    PUBLIC
    ptpn_petri
)

# Deterministic core-allocation explorer: evaluates candidate assignments with
# the exact state-class analysis and reports feasible/Pareto allocations.
add_library(ptpn_allocation STATIC
    src/analysis/allocation/allocation.cpp
)
target_link_libraries(ptpn_allocation
    PUBLIC
    ptpn_tdg
    PRIVATE
    ptpn_metrics
    ptpn_lowering
    nlohmann_json::nlohmann_json
    spdlog::spdlog
)

# Application layer: CLI-free pipelines and command entry points. The layer is
# split into four cohesive targets so `check` only needs the input/support
# layer, while pipeline, analysis and allocation stay out of its link interface.
#
# Shared options, legacy diagnostics rendering, memory accounting and the
# timing/log helpers.
add_library(ptpn_app_support STATIC
    src/app/common.cpp
    src/app/input.cpp
)
target_include_directories(ptpn_app_support PUBLIC src)
target_link_libraries(ptpn_app_support
    PUBLIC
    ptpn_diagnostics
    ptpn_dsl
    ptpn_tdg
    ptpn_types
    spdlog::spdlog
    PRIVATE
    ptpn_json
)

# `ptpn check`: parse and validate only; deliberately independent of lowering,
# analysis and allocation.
add_library(ptpn_app_check STATIC
    src/app/check_cmd.cpp
)
target_link_libraries(ptpn_app_check
    PUBLIC
    ptpn_app_support
    PRIVATE
    nlohmann_json::nlohmann_json
)
# The check path must stay independent of lowering, analysis and allocation.
get_target_property(_ptpn_app_check_interface ptpn_app_check INTERFACE_LINK_LIBRARIES)
if(_ptpn_app_check_interface MATCHES "ptpn_(allocation|lowering|metrics|reachability)")
    message(FATAL_ERROR "ptpn_app_check must not pull analysis/allocation dependencies")
endif()

# Lowering adaptation, analysis orchestration/result diagnostics, WCET export
# and the TDG/RTSL/PTPN pipeline entry points.
add_library(ptpn_app_pipelines STATIC
    src/app/analysis.cpp
    src/app/lowering.cpp
    src/app/pipelines.cpp
)
target_link_libraries(ptpn_app_pipelines
    PUBLIC
    ptpn_app_support
    PRIVATE
    ptpn_dsl
    ptpn_export
    ptpn_json
    ptpn_lowering
    ptpn_metrics
    ptpn_parser
    ptpn_petri
    ptpn_reachability
    ptpn_reductions
    ptpn_tdg
    nlohmann_json::nlohmann_json
    spdlog::spdlog
)

# Core-allocation driver. Kept separate so the check path never links the
# allocation search or its lowering/metrics dependencies.
add_library(ptpn_app_allocation STATIC
    src/app/allocation_cmd.cpp
)
target_link_libraries(ptpn_app_allocation
    PUBLIC
    ptpn_allocation
    ptpn_app_support
    PRIVATE
    ptpn_json
    ptpn_tdg
    spdlog::spdlog
)

# Aggregate facade kept for the executables and tests.
add_library(PTPN_core INTERFACE)
target_link_libraries(PTPN_core
    INTERFACE
    ptpn_types
    ptpn_diagnostics
    ptpn_dsl
    ptpn_json
    ptpn_tdg
    ptpn_petri
    ptpn_export
    ptpn_lowering
    ptpn_parser
    ptpn_dbm
    ptpn_reachability
    ptpn_metrics
    ptpn_reductions
    ptpn_allocation
    ptpn_app_support
    ptpn_app_check
    ptpn_app_pipelines
    ptpn_app_allocation
    spdlog::spdlog
)

# CLI layer: CLI11 option registration and dispatch for the `ptpn`
# subcommands. It maps CLI strings into the option structs from app/pipeline.h.
add_library(ptpn_cli STATIC
    src/cli/option_groups.cpp
    src/cli/commands/tdg_cmd.cpp
    src/cli/commands/rtsl_cmd.cpp
    src/cli/commands/ptpn_cmd.cpp
    src/cli/commands/check_cmd.cpp
    src/cli/commands/allocate_cmd.cpp
)
target_include_directories(ptpn_cli PUBLIC src)
target_link_libraries(ptpn_cli
    PUBLIC
    ptpn_app_pipelines
    ptpn_app_allocation
    ptpn_app_check
    CLI11::CLI11
)

# Main executable: diagnostics envelope scaffolding and subcommand dispatch.
add_executable(ptpn
    src/cli/main.cpp
)
target_link_libraries(ptpn PRIVATE ptpn_cli CLI11::CLI11 spdlog::spdlog)

# Test subdirectory
add_subdirectory(test)

# Expose compile_commands.json at repo root for IDE/clangd
if(CMAKE_EXPORT_COMPILE_COMMANDS)
    set(_compile_commands_link "${CMAKE_SOURCE_DIR}/compile_commands.json")
    if(EXISTS "${_compile_commands_link}" AND NOT IS_SYMLINK "${_compile_commands_link}")
        file(REMOVE "${_compile_commands_link}")
    endif()
    if(NOT EXISTS "${_compile_commands_link}")
        file(CREATE_LINK
            "${CMAKE_BINARY_DIR}/compile_commands.json"
            "${_compile_commands_link}"
            SYMBOLIC
        )
    endif()
endif()
