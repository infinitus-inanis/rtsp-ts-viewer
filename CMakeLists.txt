cmake_minimum_required(VERSION 3.22.1)
project(rtsp-ts-viewer)

option(ASAN_STATIC "enable address sanitizer" OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(glib REQUIRED IMPORTED_TARGET glib-2.0)
pkg_check_modules(gstreamer REQUIRED IMPORTED_TARGET gstreamer-1.0)
pkg_check_modules(gstreamer-rtp REQUIRED IMPORTED_TARGET gstreamer-rtp-1.0)

set(target ${PROJECT_NAME})
add_executable(${target} main.c)

target_link_libraries(${target}
  PkgConfig::glib
  PkgConfig::gstreamer
  PkgConfig::gstreamer-rtp
)

if (ASAN_STATIC)
  target_compile_options(${target}
    PRIVATE
      -Wall -Wextra -Wconversion -O0 -g -ggdb -gdwarf
      -fsanitize=address -fno-omit-frame-pointer -static-libasan
  )
  target_link_libraries(${target} libasan.a m)
endif()