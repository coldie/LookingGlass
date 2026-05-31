cmake_minimum_required(VERSION 3.10)
project(capture LANGUAGES C)

include(PreCapture)
include(CheckIncludeFile)

option(USE_D12              "Enable DirectX12 Support" ON)
option(USE_WGC              "Enable Windows Graphics Capture Support" ON)
option(USE_DXGI             "Enable DXGI Support" ON)
option(USE_NVFBC            "Enable NVFBC Support" OFF)
option(USE_D12_WGC_BACKEND  "Expose the legacy D12+WGC pipeline as Capture_D12WGC (needed only for HDR / RGB24 / downsample effects)" OFF)

if(NOT DEFINED NVFBC_SDK)
  set(NVFBC_SDK "C:/Program Files (x86)/NVIDIA Corporation/NVIDIA Capture SDK")
endif()

file(TO_CMAKE_PATH "${NVFBC_SDK}" nvfbc_sdk)

check_include_file("roapi.h" HAVE_WGC_ROAPI_H)
check_include_file("windows.graphics.capture.h" HAVE_WGC_CAPTURE_H)
check_include_file("windows.graphics.capture.interop.h" HAVE_WGC_CAPTURE_INTEROP_H)
check_include_file("windows.graphics.directx.h" HAVE_WGC_DIRECTX_H)
check_include_file("windows.graphics.directx.direct3d11.h" HAVE_WGC_DIRECT3D11_H)
find_library(RUNTIMEOBJECT_LIB runtimeobject)

if(NOT HAVE_WGC_ROAPI_H OR
   NOT HAVE_WGC_CAPTURE_H OR
   NOT HAVE_WGC_CAPTURE_INTEROP_H OR
   NOT HAVE_WGC_DIRECTX_H OR
   NOT HAVE_WGC_DIRECT3D11_H OR
   NOT RUNTIMEOBJECT_LIB)
  message("Disabling WGC support, can't find the required WinRT headers/libraries")
  set(USE_WGC OFF)
endif()

if(NOT EXISTS "${nvfbc_sdk}/inc" OR NOT IS_DIRECTORY "${nvfbc_sdk}/inc")
  message("Disabling NVFBC support, can't find the SDK headers")
  set(USE_NVFBC OFF)
endif()

# NOTE: the order here denotes the default
if(USE_D12)
  add_capture("D12")
endif()

if(USE_DXGI)
  add_capture("DXGI")
endif()

if(USE_NVFBC)
  add_capture("NVFBC")
endif()

if(USE_WGC)
  if(NOT USE_D12)
    message("Disabling WGC support, it depends on the D12 capture path")
    set(USE_WGC OFF)
  else()
    add_capture("WGC")
    if(USE_D12_WGC_BACKEND)
      list(APPEND CAPTURE "D12WGC")
      list(APPEND CAPTURE_LINK "capture_D12")
    endif()
  endif()
endif()

add_feature_info(USE_D12             USE_D12             "DirectX12 capture backend.")
add_feature_info(USE_WGC             USE_WGC             "Windows Graphics Capture backend (GPU publish).")
add_feature_info(USE_D12_WGC_BACKEND USE_D12_WGC_BACKEND "Legacy D12+WGC backend (Capture_D12WGC).")
add_feature_info(USE_DXGI            USE_DXGI            "DXGI Desktop Duplication capture backend.")
add_feature_info(USE_NVFBC           USE_NVFBC           "NVFBC capture backend.")

include("PostCapture")

add_library(capture STATIC ${CAPTURE_C})
target_link_libraries(capture ${CAPTURE_LINK})
