cmake_minimum_required(VERSION 3.5)

# Prefer the environment's VITASDK, fall back to the default install
# path so a bare `cmake ..` works on most dev boxes.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    set(CMAKE_TOOLCHAIN_FILE "/usr/local/vitasdk/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  endif()
endif()

project(pngshot-sys-screenuploader)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -Wall -O3 -nostdlib -std=c99 -fno-strict-aliasing")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-rtti -fno-exceptions")

add_executable(pngshot-ssu
  main.c
  log.c
  config.c
  uploader.c
)

target_link_libraries(pngshot-ssu
  png16
  z
  k
  m
  gcc
  taihen_stub
  ScePaf_stub_weak
  SceSysmem_stub
  # Weak stubs: SceShell already imports most of these, and we don't
  # want the taiHEN loader to refuse to load us if the host process
  # hasn't declared some symbol we reference. At runtime the real
  # Sony sysmodules resolve the stubs regardless.
  SceLibKernel_stub_weak
  SceKernelThreadMgr_stub_weak   # sceKernel{DelayThread,LockMutex,UnlockMutex,SignalSema,WaitSema,...}
  SceIofilemgr_stub_weak
  SceNet_stub_weak
  SceNetCtl_stub_weak
  SceHttp_stub_weak
  SceSsl_stub_weak               # only for SCE_HTTPS_FLAG_* + the accept-all callback signature
  SceSysmodule_stub_weak
  SceRtc_stub_weak
  SceCommonDialog_stub_weak      # SceMsgDialog used for upload-result popup
)

vita_create_self(pngshot-ssu.suprx pngshot-ssu CONFIG exports.yml UNSAFE)
