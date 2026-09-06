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

# ---------------------------------------------------------------------------
# censr-core — the redaction engine (detection worker, OCR backends, secret
# rules, QR). It has NO libobs dependency (logging goes through censr_log), so
# it is linked by both the OBS plugin and the future standalone virtual-camera
# host. See docs/roadmap-standalone-vcam.md.
# ---------------------------------------------------------------------------
add_library(
  censr-core STATIC
  src/censr_detector.cpp
  src/secret_rules.cpp
  src/qr_zxing.cpp
  src/censr_log.cpp
)
target_include_directories(censr-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
set_target_properties(
  censr-core
  PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

# Platform OCR backend: Windows.Media.Ocr (+ optional ONNX GPU OCR) on Windows,
# Vision on macOS. All implement ICensrOcr; the rest of the pipeline is neutral.
if(WIN32)
  target_sources(censr-core PRIVATE src/ocr_winrt.cpp src/onnx_ocr.cpp)
elseif(APPLE)
  target_sources(censr-core PRIVATE src/ocr_vision.mm)
  set_source_files_properties(src/ocr_vision.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
  target_link_libraries(
    censr-core
    PRIVATE "-framework Vision" "-framework CoreGraphics" "-framework Foundation"
  )
endif()

# The OBS plugin proper: OBS glue only; the engine comes from censr-core.
target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.c
    src/censr-filter.cpp
)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE censr-core)

set_target_properties(
  ${CMAKE_PROJECT_NAME}
  PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

# --- C++/WinRT (Windows.Media.Ocr) — Windows only ---
if(WIN32)
  # The projection headers ship with the Windows SDK under Include/<ver>/cppwinrt.
  file(GLOB _censr_cppwinrt_dirs "C:/Program Files (x86)/Windows Kits/10/Include/*/cppwinrt")
  list(SORT _censr_cppwinrt_dirs)
  list(REVERSE _censr_cppwinrt_dirs)
  if(_censr_cppwinrt_dirs)
    list(GET _censr_cppwinrt_dirs 0 _censr_cppwinrt)
    target_include_directories(censr-core PRIVATE "${_censr_cppwinrt}")
    message(STATUS "Censr: C++/WinRT headers at ${_censr_cppwinrt}")
  else()
    message(FATAL_ERROR "Censr: could not find C++/WinRT (cppwinrt) headers in the Windows SDK")
  endif()
  target_link_libraries(censr-core PRIVATE WindowsApp)
endif()

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
# Link ZXing statically into the plugin. On macOS a shared ZXing builds as a dylib
# whose symbols are hidden (undefined at link); a static lib pulls them in, and it
# matches how the Windows build links ZXing — no extra dylib to ship either.
set(_censr_saved_shared ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF)
FetchContent_MakeAvailable(ZXing)
set(BUILD_SHARED_LIBS ${_censr_saved_shared})
set(CMAKE_COMPILE_WARNING_AS_ERROR ${_censr_saved_wae})
# ZXing isn't warning-clean; under the Xcode generator the WARNING_AS_ERROR toggle
# above doesn't reach its targets, so silence it on the target itself.
if(TARGET ZXing)
  set_target_properties(ZXing PROPERTIES COMPILE_WARNING_AS_ERROR OFF)
  if(APPLE)
    target_compile_options(ZXing PRIVATE -Wno-shorten-64-to-32 -Wno-error)
  endif()
endif()
target_link_libraries(censr-core PRIVATE ZXing::ZXing)

# --- ONNX Runtime for GPU OCR (Windows only; macOS uses Vision) ---
if(WIN32)
# Default backend is DirectML (any DX12 GPU, no CUDA install). Set
# -DCENSR_ORT_GPU=ON to instead link the CUDA/TensorRT ONNX Runtime build
# (third_party/ort-gpu); requires CUDA + cuDNN (+ TensorRT) installed at runtime.
option(CENSR_ORT_GPU "Use the CUDA/TensorRT ONNX Runtime build instead of DirectML" OFF)
if(CENSR_ORT_GPU)
  set(CENSR_ORT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ort-gpu")
  target_compile_definitions(censr-core PRIVATE CENSR_ORT_GPU)
  set(_censr_ort_label "CUDA/TensorRT")
else()
  set(CENSR_ORT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ort-directml")
  set(_censr_ort_label "DirectML")
endif()
if(EXISTS "${CENSR_ORT_DIR}/build/native/include/onnxruntime_cxx_api.h")
  target_include_directories(censr-core PRIVATE
    "${CENSR_ORT_DIR}/build/native/include")
  target_link_libraries(censr-core PRIVATE
    "${CENSR_ORT_DIR}/runtimes/win-x64/native/onnxruntime.lib")
  message(STATUS "Censr: ONNX Runtime (${_censr_ort_label}) at ${CENSR_ORT_DIR}")
else()
  message(FATAL_ERROR "Censr: ONNX Runtime (${_censr_ort_label}) not found at ${CENSR_ORT_DIR} — run tools/fetch_gpu_ocr.py")
endif()

# Bundle the ONNX Runtime runtime DLLs with the plugin (install + rundir), so the
# packaged plugin is self-contained next to censr.dll.
set(_censr_ort_dlls "${CENSR_ORT_DIR}/runtimes/win-x64/native/onnxruntime.dll")
if(CENSR_ORT_GPU)
  file(GLOB _censr_prov_dlls "${CENSR_ORT_DIR}/runtimes/win-x64/native/onnxruntime_providers_*.dll")
  list(APPEND _censr_ort_dlls ${_censr_prov_dlls})
else()
  list(APPEND _censr_ort_dlls
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/directml/bin/x64-win/DirectML.dll")
endif()
install(FILES ${_censr_ort_dlls}
  DESTINATION "${CMAKE_PROJECT_NAME}/bin/64bit" COMPONENT Runtime)
# Also drop the ORT DLLs next to the rundir binary for local testing. Create the
# dir first (it may not exist yet on a clean build) and copy all in one command.
add_custom_command(
  TARGET ${CMAKE_PROJECT_NAME}
  POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/rundir/$<CONFIG>"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${_censr_ort_dlls}
    "${CMAKE_CURRENT_BINARY_DIR}/rundir/$<CONFIG>"
  VERBATIM
)
endif() # WIN32 (ONNX Runtime)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
