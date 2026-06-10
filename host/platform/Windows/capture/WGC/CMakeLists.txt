cmake_minimum_required(VERSION 3.10)
project(capture_WGC LANGUAGES C)

add_library(capture_WGC STATIC
  src/wgc_capture.c
  backend/wgc.c
  backend/wgc_stats.c
  backend/wgc_util.c
)

target_link_libraries(capture_WGC
  lg_common
  capture_D12
  platform_Windows
  d3d11
  d3dcompiler
  dxgi
  dwmapi
  ${RUNTIMEOBJECT_LIB}
)

target_include_directories(capture_WGC
  PRIVATE
    .
    src
    "${PROJECT_TOP}/host/platform/Windows/capture/D12"
    "${PROJECT_TOP}/vendor/directx"
)
