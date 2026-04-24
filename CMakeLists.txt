cmake_minimum_required(VERSION 3.5)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    set(CMAKE_TOOLCHAIN_FILE "/usr/local/vitasdk/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  endif()
endif()

project(pngshot)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -Wall -O3 -nostdlib -std=c99 -fno-strict-aliasing")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti -fno-exceptions")

add_executable(pngshot
  main.c
  log.c
)

target_link_libraries(pngshot
  png16
  z
  k
  m
  gcc
  taihen_stub
  ScePaf_stub_weak
  SceSysmem_stub
  # sceClibPrintf / sceClibVsnprintf live in SceLibKernel. Use the
  # weak stub so taiHEN's loader is happy even if the host process
  # (SceShell) doesn't already import SceLibKernel — the sysmodule
  # will be available at runtime anyway.
  SceLibKernel_stub_weak
)

vita_create_self(pngshot.suprx pngshot CONFIG exports.yml UNSAFE)
