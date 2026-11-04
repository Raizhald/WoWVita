# PS Vita build of the network test client: the shared net/ sources + net/tools/w3dlogin.cpp,
# packaged as a VPK. Needs VitaSDK (VITASDK set):
#
#   cmake -S vita/w3dlogin -B build-vita-login
#   cmake --build build-vita-login -j      -> build-vita-login/w3dlogin.vpk
cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(w3dlogin_vita CXX C)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "WoW3DS net test")
set(VITA_TITLEID  "WOW3D0001")
set(VITA_VERSION  "01.00")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -Wall")

set(NET ${CMAKE_CURRENT_SOURCE_DIR}/../../net)
set(NET_SOURCES
  ${NET}/client.cpp ${NET}/crypto.cpp ${NET}/socket.cpp ${NET}/inflate.cpp ${NET}/movement.cpp
  ${NET}/objects.cpp ${NET}/transport.cpp ${NET}/game.cpp ${NET}/game_social.cpp ${NET}/game_spells.cpp
  ${NET}/game_items.cpp ${NET}/game_life.cpp ${NET}/result_text.cpp ${NET}/factions.cpp ${NET}/game_factions.cpp
  ${NET}/game_vendor.cpp ${NET}/game_quests.cpp ${NET}/game_group.cpp ${NET}/game_pet.cpp ${NET}/game_friends.cpp
  ${NET}/game_services.cpp ${NET}/game_mail.cpp ${NET}/game_auction.cpp ${NET}/game_trade.cpp ${NET}/game_water.cpp
  ${NET}/tables.cpp)

add_executable(w3dlogin vita_main.cpp ${NET}/tools/w3dlogin.cpp ${NET_SOURCES})
target_include_directories(w3dlogin PRIVATE ${NET})
# The PC tool's main() becomes a function the Vita entry point calls with arguments read from a file.
set_source_files_properties(${NET}/tools/w3dlogin.cpp PROPERTIES COMPILE_DEFINITIONS "main=w3dlogin_main")

target_link_libraries(w3dlogin
  SceNet_stub SceNetCtl_stub SceSysmodule_stub SceLibKernel_stub SceKernelThreadMgr_stub
  SceIofilemgr_stub ScePower_stub SceAppUtil_stub)

vita_create_self(w3dlogin.self w3dlogin UNSAFE)
vita_create_vpk(w3dlogin.vpk ${VITA_TITLEID} w3dlogin.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME})
