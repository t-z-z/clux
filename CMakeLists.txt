cmake_minimum_required(VERSION 3.15)

if(NOT DEFINED CMAKE_CXX_COMPILER)
    find_program(CLANGPP NAMES clang++ clang-cl)
    if(CLANGPP)
        set(CMAKE_CXX_COMPILER "${CLANGPP}" CACHE FILEPATH "C++ compiler" FORCE)
    endif()
endif()

project(clux CXX)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 默认构建类型为 Debug（未指定时生效）
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
        Debug Release RelWithDebInfo MinSizeRel)
endif()

# 可执行文件输出到 bin/ 目录（bin/clux）
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
# 多配置生成器（如 Visual Studio）会追加配置子目录，这里统一压平到 bin/
foreach(_cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    string(TOUPPER ${_cfg} _CFG)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_cfg} ${CMAKE_SOURCE_DIR}/bin)
endforeach()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-Wall -Wextra)
endif()

file(GLOB SOURCES CONFIGURE_DEPENDS src/*.cpp)

add_executable(clux ${SOURCES})

target_include_directories(clux PRIVATE include)
