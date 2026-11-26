cmake_minimum_required(VERSION 3.14)
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()
project(barq LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(BARQ_BUILD_CLI "Build command-line executable" ON)
option(BARQ_BUILD_JNI "Build Android JNI bridge" OFF)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
    add_compile_options(/O2 /Ob3 /Oi /Ot /GL /W3 /utf-8)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        add_compile_options(/arch:AVX2)
    endif()
    add_link_options(/LTCG)
else()
    if(ANDROID)
        add_compile_options(-O3 -flto -ffunction-sections -fdata-sections -fvisibility=hidden -fvisibility-inlines-hidden)
        add_link_options(-flto -Wl,--gc-sections -Wl,-z,max-page-size=16384)
    else()
        add_compile_options(-O3 -Wall -Wextra -fvisibility=hidden)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
            add_compile_options(-march=x86-64-v3 -flto)
            add_link_options(-flto)
        endif()
    endif()
    if(NOT WIN32 AND NOT ANDROID)
        add_compile_options(-pthread)
        add_link_options(-pthread)
    endif()
endif()

set(BARQ_SOURCES
    cpp/src/diacritizer.cpp
    cpp/src/c_api.cpp
)

if(BARQ_BUILD_JNI OR ANDROID)
    list(APPEND BARQ_SOURCES cpp/src/jni_bridge.cpp)
endif()

add_library(barq SHARED ${BARQ_SOURCES})
target_include_directories(barq PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/cpp/include)

if(WIN32)
    target_compile_definitions(barq PRIVATE TASHKEEL_EXPORTS BARQ_EXPORTS)
    set_target_properties(barq PROPERTIES PREFIX "" OUTPUT_NAME "barq")
endif()

if(BARQ_BUILD_CLI AND NOT ANDROID)
    add_executable(barq_cli
        cpp/src/diacritizer.cpp
        cpp/src/cli.cpp
    )
    target_include_directories(barq_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cpp/include)
    set_target_properties(barq_cli PROPERTIES OUTPUT_NAME "barq")
endif()
