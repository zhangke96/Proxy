cmake_minimum_required(VERSION 3.0)

SET (CMAKE_CXX_COMPILER "/usr/bin/g++-7")

project(proxy)

set(PROXY_VERSION 0.0.1)

set(CMAKE_CXX_FLAGS
    "${CMAKE_CXX_FLAGS} -std=c++11 -Wall -fPIC"
)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "Release")
endif()

# option
option(WITH_CCACHE "Build with ccache." ON)
if(WITH_CCACHE)
  find_program(CCACHE_FOUND ccache)
  if(CCACHE_FOUND)
    if(DEFINED ENV{CCACHE_DIR})
      set(CCACHE_DIR $ENV{CCACHE_DIR})
    else(DEFINED ENV{CCACHE_DIR})
      set(CCACHE_DIR "$ENV{HOME}/.ccache")
    endif(DEFINED ENV{CCACHE_DIR})
    message(
      STATUS "Building with ccache: ${CCACHE_FOUND}, CCACHE_DIR=${CCACHE_DIR}")
    set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE ccache)
    # ccache does not accelerate link (ld), but let it handle it. by passing it
    # along with cc to python's distutils, we are able to workaround
    # https://bugs.python.org/issue8027.
    set_property(GLOBAL PROPERTY RULE_LAUNCH_LINK ccache)
  else(CCACHE_FOUND)
    message(FATAL_ERROR "Can't find ccache. Is it installed?")
  endif(CCACHE_FOUND)
endif(WITH_CCACHE)

include_directories(SYSTEM "muduo")
include_directories(SYSTEM "${CMAKE_SOURCE_DIR}")
add_subdirectory(common)
add_subdirectory(muduo)
set(muduo_deps muduo_net muduo_base)
add_subdirectory(server)
add_subdirectory(client)
