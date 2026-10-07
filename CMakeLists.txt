cmake_minimum_required(VERSION 3.27)

# vcpkg toolchain must be set before project()
if(CMAKE_TOOLCHAIN_FILE MATCHES "[/\\\\]vcpkg\\.cmake$")
    message(STATUS "Using vcpkg toolchain: ${CMAKE_TOOLCHAIN_FILE}")
else()
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

add_library(ptpn_json STATIC
    src/parse/json.cpp
)
target_link_libraries(ptpn_json
    PUBLIC
    ptpn_types
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
)
target_link_libraries(ptpn_export
    PUBLIC
    ptpn_petri
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
)
target_link_libraries(ptpn_lowering
    PUBLIC
    ptpn_petri
    ptpn_tdg
    PRIVATE
    spdlog::spdlog
)

add_library(ptpn_romeo STATIC
    src/lower/tdg2romeo/cts_writer.cpp
    src/lower/tdg2romeo/encode_common.cpp
    src/lower/tdg2romeo/inhibitor_arc.cpp
    src/lower/tdg2romeo/scheduling_net.cpp
    src/lower/tdg2romeo/tdg2romeo.cpp
    src/lower/tdg2romeo/tdg_helpers.cpp
)
target_link_libraries(ptpn_romeo
    PUBLIC
    ptpn_tdg
    PRIVATE
    Boost::filesystem
    spdlog::spdlog
)

add_library(ptpn_ptopner STATIC
    src/lower/tdg2ptopner/validate.cpp
    src/lower/tdg2ptopner/export_ppn.cpp
    src/lower/tdg2ptopner/ptpn_to_ppn.cpp
    src/lower/tdg2ptopner/tdg2ptopner.cpp
)
target_link_libraries(ptpn_ptopner
    PUBLIC
    ptpn_petri
    ptpn_tdg
    PRIVATE
    ptpn_lowering
    spdlog::spdlog
)

add_library(ptpn_parser STATIC
    src/parse/ptpn_parser.cpp
)
target_link_libraries(ptpn_parser
    PUBLIC
    ptpn_petri
)

add_library(ptpn_analysis STATIC
    src/analysis/dbm.cpp
    src/analysis/state_class.cpp
    src/analysis/ptpn_analysis.cpp
    src/analysis/canonicalization.cpp
    src/analysis/scheduling.cpp
    src/analysis/metrics.cpp
)
target_link_libraries(ptpn_analysis
    PUBLIC
    ptpn_petri
    Boost::graph
    PRIVATE
    spdlog::spdlog
)

# Aggregate facade kept for the executables and tests.
add_library(PTPN_core INTERFACE)
target_link_libraries(PTPN_core
    INTERFACE
    ptpn_types
    ptpn_json
    ptpn_tdg
    ptpn_petri
    ptpn_export
    ptpn_lowering
    ptpn_romeo
    ptpn_ptopner
    ptpn_parser
    ptpn_analysis
    spdlog::spdlog
    fmt::fmt
)

# Main executable
add_executable(ptpn src/app/main.cpp)
target_include_directories(ptpn PUBLIC src)
target_link_libraries(ptpn PRIVATE PTPN_core CLI11::CLI11)

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
