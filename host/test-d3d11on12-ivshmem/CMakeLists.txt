cmake_minimum_required(VERSION 3.10)
project(test-d3d11on12-ivshmem LANGUAGES C)

include(CheckIncludeFile)

# Standalone validation tool for the IVSHMEM-direct buffer design.
#
# Validates that:
#   1. ID3D12Device3::OpenExistingHeapFromAddress works on IVSHMEM
#   2. CreatePlacedResource with TEXTURE2D + ROW_MAJOR succeeds in that heap
#   3. D3D11On12CreateDevice + CreateWrappedResource produces a usable
#      ID3D11Texture2D backed by the placed D3D12 resource
#   4. D3D11 CopyResource into the wrapped texture lands the right bytes
#      in the IVSHMEM region (verified by reading back via the BAR mapping)
#   5. Sustained throughput is acceptable for the per-frame copy budget

add_compile_definitions(WINVER=0x0601 _WIN32_WINNT=0x0601 COBJMACROS INITGUID)

if(BUILD_TEST_D3D11ON12_IVSHMEM)
  add_executable(test-d3d11on12-ivshmem
    src/main.c
  )

  target_link_libraries(test-d3d11on12-ivshmem
    lg_common
    platform_Windows
    d3d11
    d3d12
    dxgi
    dxguid
    setupapi
  )

  target_include_directories(test-d3d11on12-ivshmem
    PRIVATE
      "${PROJECT_TOP}/vendor/directx"
  )
endif()

if(BUILD_TEST_WGC_D3D11ON12_FRAMEPOOL)
  check_include_file("roapi.h" HAVE_TEST_WGC_ROAPI_H)
  check_include_file("windows.graphics.capture.h" HAVE_TEST_WGC_CAPTURE_H)
  check_include_file("windows.graphics.capture.interop.h" HAVE_TEST_WGC_CAPTURE_INTEROP_H)
  check_include_file("windows.graphics.directx.h" HAVE_TEST_WGC_DIRECTX_H)
  check_include_file("windows.graphics.directx.direct3d11.h" HAVE_TEST_WGC_DIRECT3D11_H)
  find_library(TEST_WGC_RUNTIMEOBJECT_LIB runtimeobject)

  if(NOT HAVE_TEST_WGC_ROAPI_H OR
     NOT HAVE_TEST_WGC_CAPTURE_H OR
     NOT HAVE_TEST_WGC_CAPTURE_INTEROP_H OR
     NOT HAVE_TEST_WGC_DIRECTX_H OR
     NOT HAVE_TEST_WGC_DIRECT3D11_H OR
     NOT TEST_WGC_RUNTIMEOBJECT_LIB)
    message(FATAL_ERROR
      "BUILD_TEST_WGC_D3D11ON12_FRAMEPOOL requires WinRT WGC headers and runtimeobject")
  endif()

  add_executable(test-wgc-d3d11on12-framepool
    src/wgc_d3d11on12_framepool.c
  )

  target_link_libraries(test-wgc-d3d11on12-framepool
    d3d11
    d3d12
    dxgi
    dxguid
    ${TEST_WGC_RUNTIMEOBJECT_LIB}
  )

  target_include_directories(test-wgc-d3d11on12-framepool
    PRIVATE
      "${PROJECT_TOP}/vendor/directx"
  )
endif()
