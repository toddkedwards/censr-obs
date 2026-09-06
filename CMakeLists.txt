cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.c
    src/censr-filter.cpp
    src/censr_detector.cpp
    src/ocr_winrt.cpp
    src/onnx_ocr.cpp
    src/secret_rules.cpp
    src/qr_zxing.cpp
)

set_target_properties(
  ${CMAKE_PROJECT_NAME}
  PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

# --- C++/WinRT (Windows.Media.Ocr) ---
# The projection headers ship with the Windows SDK under Include/<ver>/cppwinrt.
file(GLOB _censr_cppwinrt_dirs "C:/Program Files (x86)/Windows Kits/10/Include/*/cppwinrt")
list(SORT _censr_cppwinrt_dirs)
list(REVERSE _censr_cppwinrt_dirs)
if(_censr_cppwinrt_dirs)
  list(GET _censr_cppwinrt_dirs 0 _censr_cppwinrt)
  target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE "${_censr_cppwinrt}")
  message(STATUS "Censr: C++/WinRT headers at ${_censr_cppwinrt}")
else()
  message(FATAL_ERROR "Censr: could not find C++/WinRT (cppwinrt) headers in the Windows SDK")
endif()
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE WindowsApp)

# --- ZXing-cpp (QR / barcode detection) via FetchContent ---
include(FetchContent)
set(ZXING_READERS ON CACHE BOOL "" FORCE)
set(ZXING_WRITERS OFF CACHE BOOL "" FORCE)
set(ZXING_EXPERIMENTAL_API OFF CACHE BOOL "" FORCE)
set(ZXING_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZXING_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(ZXING_BLACKBOX_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  ZXing
  GIT_REPOSITORY https://github.com/zxing-cpp/zxing-cpp.git
  GIT_TAG v2.3.0
  GIT_SHALLOW ON
)
# The template defaults CMAKE_COMPILE_WARNING_AS_ERROR to ON; ZXing is not
# warning-clean under MSVC, so build it without /WX.
set(_censr_saved_wae ${CMAKE_COMPILE_WARNING_AS_ERROR})
set(CMAKE_COMPILE_WARNING_AS_ERROR OFF)
FetchContent_MakeAvailable(ZXing)
set(CMAKE_COMPILE_WARNING_AS_ERROR ${_censr_saved_wae})
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ZXing::ZXing)

# --- ONNX Runtime (DirectML) for GPU OCR ---
set(CENSR_ORT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ort-directml")
if(EXISTS "${CENSR_ORT_DIR}/build/native/include/onnxruntime_cxx_api.h")
  target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    "${CENSR_ORT_DIR}/build/native/include")
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE
    "${CENSR_ORT_DIR}/runtimes/win-x64/native/onnxruntime.lib")
  message(STATUS "Censr: ONNX Runtime (DirectML) at ${CENSR_ORT_DIR}")
else()
  message(FATAL_ERROR "Censr: ONNX Runtime not found at ${CENSR_ORT_DIR} (see third_party)")
endif()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
