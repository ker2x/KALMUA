cmake_minimum_required(VERSION 3.21)
project(KALMUA CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /permissive-  /Zc:__cplusplus /utf-8 /Ox)
    add_compile_definitions(
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        UNICODE
        _UNICODE
    )
endif()

add_executable(KALMUA
    src/main.cpp
    src/ffb/di8_device.cpp
    src/ffb/di8_device.hpp
    src/SharedMemoryInterface/SharedMemoryInterface.hpp
    src/SharedMemoryInterface/InternalsPlugin.hpp
    src/SharedMemoryInterface/PluginObjects.hpp
)

target_include_directories(KALMUA PRIVATE src)

# Studio 397 SDK headers are vendored as-is. Treat them as external so we
# don't flag the vendor's code at /W4.
if(MSVC)
    target_compile_options(KALMUA PRIVATE
        /external:I ${CMAKE_CURRENT_SOURCE_DIR}/src/SharedMemoryInterface
        /external:W0
    )
endif()

target_link_libraries(KALMUA PRIVATE
    dinput8
    dxguid
)
