cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VITASDK})
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
endif()

project(VitaMail C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wno-unused-parameter -fsigned-char -g")
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")

# Collect mbedTLS 2.28.x library sources directly (no POSIX net_sockets.c)
file(GLOB MBEDTLS_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls/library/*.c")
list(FILTER MBEDTLS_SOURCES EXCLUDE REGEX "net_sockets\\.c$")

add_library(vita_mbedtls STATIC ${MBEDTLS_SOURCES})
target_include_directories(vita_mbedtls PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/vita"
)
target_compile_definitions(vita_mbedtls PUBLIC
  MBEDTLS_CONFIG_FILE="vita_mbedtls_config.h"
)

set(VITAMAIL_CORE_SOURCES
  src/net_tls.c
  src/mime.c
  src/imap.c
  src/smtp.c
  src/config.c
)

if(VITA)
  include("${VITASDK}/share/vita.cmake" REQUIRED)

  set(VITA_APP_NAME "VitaMail")
  set(VITA_TITLEID  "VTML00001")
  set(VITA_VERSION  "01.00")
  set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")

  find_package(SDL2 REQUIRED)
  find_package(Freetype REQUIRED)

  add_executable(${PROJECT_NAME}
    src/main.c
    src/app.c
    src/ui.c
    src/ime.c
    vita/vita_runtime.c
    ${VITAMAIL_CORE_SOURCES}
  )

  target_include_directories(${PROJECT_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    ${SDL2_INCLUDE_DIRS}
    ${FREETYPE_INCLUDE_DIRS}
  )

  target_link_libraries(${PROJECT_NAME}
    vita_mbedtls
    SDL2::SDL2
    ${FREETYPE_LIBRARIES}
    SceNet_stub
    SceNetCtl_stub
    SceSysmodule_stub
    SceIme_stub
    SceCommonDialog_stub
    SceDisplay_stub
    SceCtrl_stub
    SceTouch_stub
    SceGxm_stub
    ScePower_stub
    SceAppUtil_stub
    SceRtc_stub
    SceIofilemgr_stub
    SceKernelDmacMgr_stub
    png
    z
    bz2
    m
    -pthread
  )

  # UNSAFE is required so VitaMail can read the iTLS-Enso CA store at vs0:/data/external/cert/CA_LIST.cer
  vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME} UNSAFE)
  vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
    FILE sce_sys/icon0.png sce_sys/icon0.png
    FILE sce_sys/pic0.png sce_sys/pic0.png
    FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
    FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
    FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
    FILE assets/font.ttf assets/font.ttf
  )
else()
  # Host unit test build
  enable_testing()
  add_executable(vitamail_tests
    tests/test_protocol.c
    ${VITAMAIL_CORE_SOURCES}
  )
  target_include_directories(vitamail_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
  )
  target_link_libraries(vitamail_tests PRIVATE
    vita_mbedtls
    pthread
  )
  add_test(NAME vitamail_tests COMMAND vitamail_tests)
endif()
