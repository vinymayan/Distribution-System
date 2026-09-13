cmake_minimum_required(VERSION 3.21)

if(NOT DEFINED ENV{COMMONLIB_SSE_FOLDER})
    message(FATAL_ERROR "Missing COMMONLIB_SSE_FOLDER environment variable")
endif()

set(AUTHOR_NAME "Viny")
set(PRODUCT_NAME "EasyDistribution")
project(${PRODUCT_NAME} VERSION 1.1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)

set(ENABLE_SKYRIM_SE ON CACHE BOOL " " FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL " " FORCE)
set(ENABLE_SKYRIM_VR ON CACHE BOOL " " FORCE)
set(BUILD_TESTS OFF CACHE BOOL " " FORCE)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(cmake/lib/copyOutputs.cmake)
include(cmake/lib/automaticGameFolderOutput.cmake)

add_subdirectory(
    "$ENV{COMMONLIB_SSE_FOLDER}"
    "${CMAKE_CURRENT_BINARY_DIR}/_deps/clib-build")
get_target_property(COMMONLIB_SRC_DIR CommonLibSSE SOURCE_DIR)
include(${COMMONLIB_SRC_DIR}/cmake/CommonLibSSE.cmake)

find_path(CLIB_UTIL_INCLUDE_DIRS "ClibUtil/utils.hpp")
find_path(CLIB_UTILS_QTR_INCLUDE_DIRS "ClibUtilsQTR/utils.hpp")
find_path(SKSE_MCP_INCLUDE_DIRS "SKSEMCP/utils.hpp")
find_package(miniz CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)

file(
    GLOB_RECURSE
    CORE_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/DistributionCore/*.cpp")
list(APPEND CORE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Manager.cpp")
file(
    GLOB_RECURSE
    INLOS_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/INLOS/*.cpp")
file(
    GLOB_RECURSE
    WIYT_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/WIYT/*.cpp")
list(APPEND WIYT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/INLOS/NewSkillMenu.cpp")
file(
    GLOB_RECURSE
    EDF_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
list(FILTER EDF_SOURCES EXCLUDE REGEX "/src/DistributionCore/")
list(FILTER EDF_SOURCES EXCLUDE REGEX "/src/INLOS/")
list(FILTER EDF_SOURCES EXCLUDE REGEX "/src/WIYT/")
list(FILTER EDF_SOURCES EXCLUDE REGEX "/src/Manager\\.cpp$")

file(
    GLOB_RECURSE
    PROJECT_HEADERS
    CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h")

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.rc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/version.rc"
    @ONLY)
set(EDF_RESOURCE_FILE "${CMAKE_CURRENT_BINARY_DIR}/version.rc")

add_library(DistributionRulesCore STATIC ${CORE_SOURCES})
target_compile_features(DistributionRulesCore PUBLIC cxx_std_23)
target_precompile_headers(
    DistributionRulesCore
    PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include/PCH.h")
target_include_directories(
    DistributionRulesCore
    PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CLIB_UTIL_INCLUDE_DIRS}"
    "${CLIB_UTILS_QTR_INCLUDE_DIRS}"
    "${SKSE_MCP_INCLUDE_DIRS}")
target_link_libraries(
    DistributionRulesCore
    PUBLIC
    CommonLibSSE::CommonLibSSE)

add_commonlibsse_plugin(
    ${PROJECT_NAME}
    NAME "EasyDistribution"
    AUTHOR "${AUTHOR_NAME}"
    VERSION "${PROJECT_VERSION}"
    SOURCES
        ${PROJECT_HEADERS}
        ${EDF_SOURCES}
        ${EDF_RESOURCE_FILE})
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_precompile_headers(
    ${PROJECT_NAME}
    PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include/PCH.h")
target_include_directories(
    ${PROJECT_NAME}
    PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CLIB_UTIL_INCLUDE_DIRS}"
    "${CLIB_UTILS_QTR_INCLUDE_DIRS}"
    "${SKSE_MCP_INCLUDE_DIRS}")
target_link_libraries(
    ${PROJECT_NAME}
    PRIVATE
    DistributionRulesCore
    miniz::miniz
    unofficial::sqlite3::sqlite3)
target_compile_definitions(${PROJECT_NAME} PRIVATE IS_HOST_PLUGIN)
set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "EDF")

add_commonlibsse_plugin(
    INLOS
    NAME "INLOS"
    AUTHOR "${AUTHOR_NAME}"
    VERSION "0.1.0.0"
    SOURCES
        ${INLOS_SOURCES})
target_compile_features(INLOS PRIVATE cxx_std_23)
target_precompile_headers(
    INLOS
    PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include/PCH.h")
target_include_directories(
    INLOS
    PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CLIB_UTIL_INCLUDE_DIRS}"
    "${CLIB_UTILS_QTR_INCLUDE_DIRS}"
    "${SKSE_MCP_INCLUDE_DIRS}")
target_link_libraries(
    INLOS
    PRIVATE
    DistributionRulesCore
    miniz::miniz
    unofficial::sqlite3::sqlite3)
target_compile_definitions(INLOS PRIVATE INLOS_PLUGIN)

add_commonlibsse_plugin(
    WIYT
    NAME "WIYT"
    AUTHOR "${AUTHOR_NAME}"
    VERSION "0.1.0.0"
    SOURCES
        ${WIYT_SOURCES})
target_compile_features(WIYT PRIVATE cxx_std_23)
target_precompile_headers(
    WIYT
    PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include/PCH.h")
target_include_directories(
    WIYT
    PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CLIB_UTIL_INCLUDE_DIRS}"
    "${CLIB_UTILS_QTR_INCLUDE_DIRS}"
    "${SKSE_MCP_INCLUDE_DIRS}")
target_link_libraries(
    WIYT
    PRIVATE
    DistributionRulesCore
    miniz::miniz
    unofficial::sqlite3::sqlite3)
target_compile_definitions(WIYT PRIVATE WIYT_PLUGIN)

add_dependencies(${PROJECT_NAME} INLOS WIYT)

source_group(
    TREE "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES
        ${PROJECT_HEADERS}
        ${EDF_SOURCES}
        ${CORE_SOURCES}
        ${INLOS_SOURCES}
        ${WIYT_SOURCES})

set(wildlander_output false)
set(steam_owrt_output false)
set(steam_mods_output true)
set(lorerim_mods_output false)
set(lorerim3_mods_output true)
automaticGameFolderOutput(
    ${PROJECT_NAME}
    "EDF"
    wildlander_output
    steam_owrt_output
    steam_mods_output
    lorerim_mods_output
    lorerim3_mods_output)
automaticGameFolderOutput(
    INLOS
    "INLOS"
    wildlander_output
    steam_owrt_output
    steam_mods_output
    lorerim_mods_output
    lorerim3_mods_output)
automaticGameFolderOutput(
    WIYT
    "WIYT"
    wildlander_output
    steam_owrt_output
    steam_mods_output
    lorerim_mods_output
    lorerim3_mods_output)
