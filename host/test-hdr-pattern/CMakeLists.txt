cmake_minimum_required(VERSION 3.10)
project(test-hdr-pattern LANGUAGES C)

add_compile_definitions(WINVER=0x0A00 _WIN32_WINNT=0x0A00 COBJMACROS INITGUID)

add_executable(test-hdr-pattern
  src/main.c
)

target_link_libraries(test-hdr-pattern
  d3d11
  dxgi
  dxguid
  d3dcompiler
)
