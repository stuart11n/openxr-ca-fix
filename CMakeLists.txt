cmake_minimum_required(VERSION 3.20)
project(openxr_ca_fix LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(LAYER_NAME XR_APILAYER_S16N_ca_fix)

# OpenXR headers only. SOURCE_SUBDIR points at a folder with no CMakeLists.txt,
# so MakeAvailable downloads the SDK without building the loader.
include(FetchContent)
FetchContent_Declare(openxr_sdk
  URL https://github.com/KhronosGroup/OpenXR-SDK/archive/refs/tags/release-1.1.38.tar.gz
  SOURCE_SUBDIR do-not-build
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(openxr_sdk)

add_library(${LAYER_NAME} SHARED
  src/layer.cpp
  src/log.cpp
  src/layer.def)
target_include_directories(${LAYER_NAME} PRIVATE ${openxr_sdk_SOURCE_DIR}/include)
target_compile_definitions(${LAYER_NAME} PRIVATE
  WIN32_LEAN_AND_MEAN NOMINMAX
  LAYER_NAME="${LAYER_NAME}")
if(MSVC)
  target_compile_options(${LAYER_NAME} PRIVATE /W4 /permissive-)
  # Static CRT so the DLL loads in any game process without redist surprises.
  set_property(TARGET ${LAYER_NAME} PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Manifest goes next to the DLL; register.ps1 points the loader at it.
configure_file(${CMAKE_SOURCE_DIR}/manifest/${LAYER_NAME}.json.in
  ${CMAKE_BINARY_DIR}/${LAYER_NAME}.json @ONLY)
add_custom_command(TARGET ${LAYER_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_BINARY_DIR}/${LAYER_NAME}.json $<TARGET_FILE_DIR:${LAYER_NAME}>)
