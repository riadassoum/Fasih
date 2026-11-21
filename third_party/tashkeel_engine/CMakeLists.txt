cmake_minimum_required(VERSION 3.14)
project(tashkeel_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(TASHKEEL_BUILD_CLI "Build command-line tool" ON)
option(TASHKEEL_BUILD_JNI "Build Android JNI bridge" OFF)

if(MSVC)
    add_compile_options(/O2 /W3 /utf-8)
else()
    add_compile_options(-O3 -Wall -Wextra -fvisibility=hidden)
endif()

set(TASHKEEL_SOURCES
    cpp/src/diacritizer.cpp
    cpp/src/c_api.cpp
)

if(TASHKEEL_BUILD_JNI OR ANDROID)
    list(APPEND TASHKEEL_SOURCES cpp/src/jni_bridge.cpp)
endif()

add_library(tashkeel SHARED ${TASHKEEL_SOURCES})
target_include_directories(tashkeel PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/cpp/include)

if(WIN32)
    set_target_properties(tashkeel PROPERTIES PREFIX "" OUTPUT_NAME "tashkeel")
endif()

if(TASHKEEL_BUILD_CLI AND NOT ANDROID)
    add_executable(tashkeel_cli
        cpp/src/diacritizer.cpp
        cpp/src/cli.cpp
    )
    target_include_directories(tashkeel_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cpp/include)
endif()
