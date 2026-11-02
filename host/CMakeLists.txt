# Host (desktop) build of the same Rive runtime + ThorVG + adapter code that runs
# on the ESP32-P4. Renders frames of a .riv to PNG files so the adapter can be
# checked on a PC without flashing.
#
#   cmake -S host -B build-host -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-host
#   ./build-host/rive_host_render main/assets/sample.riv out 10 30 720 2 1
cmake_minimum_required(VERSION 3.16)
project(rive_host_render C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
set(RIVE_DIR "${ROOT}/third_party/rive-runtime")
set(THORVG_DIR "${ROOT}/third_party/thorvg")
include("${ROOT}/components/rive_runtime/rive_sources.cmake")
include("${ROOT}/components/thorvg/thorvg_sources.cmake")

add_library(thorvg STATIC ${THORVG_SRCS})
target_include_directories(thorvg PUBLIC ${THORVG_PUBLIC_INCS} PRIVATE ${THORVG_PRIV_INCS})
target_compile_definitions(thorvg PUBLIC ${THORVG_DEFINES})
target_compile_options(thorvg PRIVATE -fno-exceptions -fno-rtti -w)

add_library(rive STATIC ${RIVE_SRCS})
target_include_directories(rive PUBLIC ${RIVE_PUBLIC_INCS})
target_compile_definitions(rive PRIVATE ${RIVE_DEFINES} PUBLIC ${RIVE_PUBLIC_DEFINES})
target_compile_options(rive PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti> -w)

file(GLOB ADAPTER_SRCS "${ROOT}/components/rive_thorvg/src/*.cpp")
add_library(rive_thorvg STATIC ${ADAPTER_SRCS})
target_include_directories(rive_thorvg PUBLIC "${ROOT}/components/rive_thorvg/include")
target_link_libraries(rive_thorvg PUBLIC rive thorvg)
target_compile_options(rive_thorvg PRIVATE -fno-exceptions -fno-rtti -Wall -Wextra -Wno-unused-parameter)

find_package(Threads REQUIRED)
add_executable(rive_host_render host_main.cpp)
target_link_libraries(rive_host_render PRIVATE rive_thorvg Threads::Threads)
