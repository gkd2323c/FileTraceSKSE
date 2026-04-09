cmake_minimum_required(VERSION 3.24)

# ========================================
# Project Configuration
# ========================================
project(
    FileTraceSKSE
    VERSION 1.0.0
    LANGUAGES CXX
)

# ========================================
# Build Options
# ========================================
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG OFF)

# MSVC-specific flags
if(MSVC)
    set(FILETRACE_MSVC_COMPILE_OPTIONS
        /MP     # Multi-processor compilation
        /W4     # Warning level 4
        /WX     # Treat warnings as errors
        /permissive-    # Standards conformance
        /Zc:preprocessor    # Use conforming preprocessor
        /EHsc   # Exception handling
        /utf-8  # UTF-8 source files

        # Disable specific warnings
        /wd4068 # unknown pragma 'clang'
        /wd4189 # local variable is initialized but not referenced
        /wd4200 # nonstandard extension used : zero-sized array in struct/union
        /wd4201 # nonstandard extension used : nameless struct/union
        /wd4324 # structure was padded due to alignment specifier
    )
endif()

# ========================================
# Dependencies - Auto-downloaded via FetchContent
# ========================================
include(FetchContent)
set(CMAKE_SKIP_INSTALL_RULES ON)

message(STATUS "Fetching fmt...")
FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG 10.2.1
    GIT_SHALLOW TRUE
    OVERRIDE_FIND_PACKAGE
)

message(STATUS "Fetching spdlog...")
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.14.1
    GIT_SHALLOW TRUE
    OVERRIDE_FIND_PACKAGE
)

message(STATUS "Fetching CommonLibSSE-NG (this may take a few minutes on first build)...")
FetchContent_Declare(
    CommonLibSSE
    GIT_REPOSITORY https://github.com/CharmedBaryon/CommonLibSSE-NG.git
    GIT_TAG v3.7.0
    GIT_SHALLOW TRUE
)

message(STATUS "Fetching MinHook...")
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG v1.3.4
    GIT_SHALLOW TRUE
)

message(STATUS "Fetching SimpleIni...")
FetchContent_Declare(
    SimpleIni
    GIT_REPOSITORY https://github.com/brofield/simpleini.git
    GIT_TAG v4.25
    GIT_SHALLOW TRUE
)

message(STATUS "Fetching rapidcsv...")
FetchContent_Declare(
    rapidcsv
    GIT_REPOSITORY https://github.com/d99kris/rapidcsv.git
    GIT_TAG v8.84
    GIT_SHALLOW TRUE
)

FetchContent_GetProperties(rapidcsv)
if(NOT rapidcsv_POPULATED)
    FetchContent_Populate(rapidcsv)
endif()
set(RAPIDCSV_INCLUDE_DIRS "${rapidcsv_SOURCE_DIR}/src" CACHE PATH "rapidcsv include directory" FORCE)

set(BUILD_TESTS OFF CACHE BOOL "Disable CommonLibSSE tests for plugin build" FORCE)
set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "Use external fmt for spdlog" FORCE)
FetchContent_MakeAvailable(fmt spdlog CommonLibSSE minhook SimpleIni)

# ========================================
# Source Files
# ========================================
set(SOURCES
    src/main.cpp
    src/PCH.h
)

# ========================================
# Build Target
# ========================================
add_library(${PROJECT_NAME} SHARED ${SOURCES})

target_include_directories(${PROJECT_NAME}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${RAPIDCSV_INCLUDE_DIRS}
)

if(MSVC)
    target_compile_options(${PROJECT_NAME}
        PRIVATE
            ${FILETRACE_MSVC_COMPILE_OPTIONS}
    )
endif()

target_link_libraries(${PROJECT_NAME}
    PRIVATE
        CommonLibSSE::CommonLibSSE
        minhook
        SimpleIni::SimpleIni
)

# ========================================
# Precompiled Headers
# ========================================
target_precompile_headers(${PROJECT_NAME}
    PRIVATE
        src/PCH.h
)

# ========================================
# Output Configuration
# ========================================
set_target_properties(${PROJECT_NAME}
    PROPERTIES
        OUTPUT_NAME "${PROJECT_NAME}"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)

# ========================================
# Post-Build: Copy to Skyrim Data/SKSE/Plugins
# ========================================
# Uncomment and set your Skyrim path to auto-deploy after build:
# set(SKYRIM_PATH "C:/Program Files (x86)/Steam/steamapps/common/Skyrim Special Edition")
# add_custom_command(
#     TARGET ${PROJECT_NAME} POST_BUILD
#     COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PROJECT_NAME}> "${SKYRIM_PATH}/Data/SKSE/Plugins/"
#     COMMAND ${CMAKE_COMMAND} -E echo "Deployed to: ${SKYRIM_PATH}/Data/SKSE/Plugins/"
# )
