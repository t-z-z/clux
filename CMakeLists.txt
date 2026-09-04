cmake_minimum_required(VERSION 3.15)
add_compile_options(/utf-8)
project(volang CXX)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# 默认使用 clang 作为编译器
if(NOT DEFINED CMAKE_CXX_COMPILER)
    find_program(CLANGPP NAMES clang++ clang-cl)
    if(CLANGPP)
        set(CMAKE_CXX_COMPILER "${CLANGPP}")
    endif()
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 可执行文件输出到 bin/ 目录（bin/volc.exe）
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

add_executable(volc ${SOURCES})

target_include_directories(volc PRIVATE include)
