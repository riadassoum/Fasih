cmake_minimum_required(VERSION 3.14)
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
    add_compile_options(/O2 /W3 /utf-8)
else()
    add_compile_options(-O3 -Wall -Wextra -fvisibility=hidden)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
        add_compile_options(-mpopcnt)
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

if(ANDROID)
    # 16 KB ELF page alignment required by Android 15+ / Google Play
    target_link_options(barq PRIVATE -Wl,-z,max-page-size=16384 -Wl,--gc-sections)
endif()

if(BARQ_BUILD_CLI AND NOT ANDROID)
    add_executable(barq_cli
        cpp/src/diacritizer.cpp
        cpp/src/cli.cpp
    )
    target_include_directories(barq_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/cpp/include)
    set_target_properties(barq_cli PROPERTIES OUTPUT_NAME "barq")
endif()
