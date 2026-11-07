cmake_minimum_required(VERSION 3.10)
project(vc4top CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_executable(vc4top vc4top.cpp)
target_link_libraries(vc4top pthread)
target_compile_options(vc4top PRIVATE -Wall -Wextra)

install(TARGETS vc4top RUNTIME DESTINATION bin)

set(CPACK_GENERATOR "DEB")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "rughinnit")
set(CPACK_PACKAGE_NAME "vc4top")
set(CPACK_PACKAGE_VERSION "0.2.0")
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "armhf")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Realtime VideoCore IV / V3D QPU monitor")
set(CPACK_PACKAGE_DESCRIPTION "Terminal btop-style monitor for the Broadcom VideoCore IV GPU/QPU. Shows system/power, QPU scheduler and VPM state, all 30 V3D performance counter sources and the full V3D register map.")
set(CPACK_DEBIAN_PACKAGE_SECTION "main")
include(CPack)
