# WoWVita: the WoW 1.12.1 client for the PS Vita — or, without the VitaSDK toolchain, the same build for a Linux PC (headless GLES,
# used to test the compatibility layer; see platform/src/platform_pc.cpp).
#
#   Vita:  cmake -S . -B build-vita -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake && cmake --build build-vita
#          -> build-vita/WoWVita.vpk
#   PC:    cmake -S . -B build-pc -DKHR_INCLUDE=<EGL/GLES headers> && cmake --build build-pc   -> build-pc/wowvita_pc
cmake_minimum_required(VERSION 3.16)

if(DEFINED ENV{VITASDK} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE AND NOT W3D_PC)
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
endif()

project(wowvita C CXX)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(COMPAT ${CMAKE_CURRENT_SOURCE_DIR}/platform)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_C_STANDARD 11)

# ------------------------------------------------------------------ shaders: .v.pica -> <name>_shbin.h (GLSL + Cg)
set(GEN ${CMAKE_CURRENT_BINARY_DIR}/gen)
file(GLOB PICA_SHADERS ${ROOT}/game/source/*.v.pica)
set(SHBIN_HEADERS)
foreach(sh ${PICA_SHADERS})
  get_filename_component(n ${sh} NAME)
  string(REPLACE ".v.pica" "" n ${n})
  add_custom_command(OUTPUT ${GEN}/${n}_shbin.h
    COMMAND ${Python3_EXECUTABLE} -I ${COMPAT}/tools/pica2glsl.py ${sh} ${GEN}
    DEPENDS ${sh} ${COMPAT}/tools/pica2glsl.py
    COMMENT "pica2glsl ${n}")
  list(APPEND SHBIN_HEADERS ${GEN}/${n}_shbin.h)
endforeach()
add_custom_target(shaders DEPENDS ${SHBIN_HEADERS})

# ------------------------------------------------------------------ sources
file(GLOB GAME_SOURCES ${ROOT}/game/source/*.cpp)
file(GLOB NET_SOURCES ${ROOT}/net/*.cpp)
file(GLOB MATHS_SOURCES ${COMPAT}/src/maths/*.c)
set(COMPAT_SOURCES
  ${COMPAT}/src/c3d_gl.cpp ${COMPAT}/src/ctru_common.cpp ${COMPAT}/src/ndsp.cpp ${COMPAT}/src/paths.cpp
  ${COMPAT}/src/layout.cpp ${COMPAT}/src/console.cpp)

if(VITA)
  include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)
  list(APPEND COMPAT_SOURCES ${COMPAT}/src/platform_vita.cpp)
  set(EXE wowvita)
else()
  list(APPEND COMPAT_SOURCES ${COMPAT}/src/platform_pc.cpp)
  set(EXE wowvita_pc)
endif()

add_executable(${EXE} ${GAME_SOURCES} ${NET_SOURCES} ${COMPAT_SOURCES} ${MATHS_SOURCES})
add_dependencies(${EXE} shaders)
target_include_directories(${EXE} PRIVATE ${COMPAT}/include ${GEN} ${ROOT}/game/source ${ROOT}/net ${ROOT}/common)
# faster world streaming: glued reads + a second IO worker sharing the descriptors (game/source/streamer.cpp)
target_compile_definitions(${EXE} PRIVATE W3D_IO_FAST=1)

# The 3DS build: -O3 -ffast-math -fno-finite-math-only, no RTTI / exceptions
set(GAME_FLAGS -O2 -ffast-math -fno-finite-math-only -Wall -Wno-psabi -Wno-unused-function -Wno-unused-variable)
target_compile_options(${EXE} PRIVATE ${GAME_FLAGS} $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti -fno-exceptions>)
target_link_options(${EXE} PRIVATE -Wl,--wrap=fopen,--wrap=open,--wrap=mkdir,--wrap=remove,--wrap=rename
  # stdout -> the touch-screen text console (console.cpp)
  -Wl,--wrap=printf,--wrap=vprintf,--wrap=puts,--wrap=putchar)

if(VITA)
  target_compile_options(${EXE} PRIVATE -mcpu=cortex-a9 -mfpu=neon)
  target_link_libraries(${EXE}
    vitaGL vitashark SceShaccCgExt mathneon taihen_stub SceShaccCg_stub SceKernelDmacMgr_stub
    SceAppMgr_stub SceAppUtil_stub SceAudio_stub SceCtrl_stub SceTouch_stub SceCommonDialog_stub SceDisplay_stub
    SceGxm_stub SceIme_stub SceNet_stub SceNetCtl_stub SceSysmodule_stub SceLibKernel_stub SceKernelThreadMgr_stub
    SceIofilemgr_stub ScePower_stub ScePgf_stub SceRtc_stub pthread m stdc++)

  set(VITA_APP_NAME "WoWVita")
  set(VITA_TITLEID "WOWV00001")
  set(VITA_VERSION "01.00")
  # 450 MB of the 512 MB for the game (vitaGL keeps textures in CDRAM where it can)
  set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")
  vita_create_self(eboot.bin ${EXE} UNSAFE)
  vita_create_vpk(WoWVita.vpk ${VITA_TITLEID} eboot.bin
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
    # a small synthetic test region (built by converter: w3dconv --synthetic)
    FILE ${ROOT}/data/synth.w3dp synth.w3dp
    FILE ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/icon0.png sce_sys/icon0.png
    FILE ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
    FILE ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
    FILE ${CMAKE_CURRENT_SOURCE_DIR}/sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml)
else()
  if(NOT KHR_INCLUDE)
    set(KHR_INCLUDE ${COMPAT}/tools/khr/include)
  endif()
  target_include_directories(${EXE} PRIVATE ${KHR_INCLUDE})
  # -U_FORTIFY_SOURCE: Ubuntu turns printf into __printf_chk, which the console wrap would not see
  target_compile_options(${EXE} PRIVATE -g -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)
  target_link_libraries(${EXE} :libEGL.so.1 :libGLESv2.so.2 pthread m)
endif()
