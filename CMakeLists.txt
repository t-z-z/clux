cmake_minimum_required(VERSION 3.15)

project(volang CXX)

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

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-Wall -Wextra)
endif()

file(GLOB SOURCES CONFIGURE_DEPENDS src/*.cpp)

add_executable(volc ${SOURCES})

target_include_directories(volc PRIVATE include)
