cmake_minimum_required(VERSION 3.10)
project(vc4top VERSION 1.0.3 LANGUAGES CXX)

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
install(FILES README.md LICENSE.txt DESTINATION share/doc/vc4top)
install(DIRECTORY docs/ DESTINATION share/doc/vc4top/docs)
install(FILES kernel/vc4mon.c kernel/Makefile kernel/README.md
        DESTINATION share/doc/vc4top/kernel)

set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_NAME "vc4top")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VENDOR "rughinnit")
set(CPACK_PACKAGE_CONTACT "rughinnit <rughinnit@users.noreply.github.com>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Realtime VideoCore IV / V3D QPU telemetry monitor")
set(CPACK_PACKAGE_DESCRIPTION "btop-style terminal telemetry monitor for the Broadcom VideoCore IV
GPU/QPU found on Raspberry Pi 1, 2, 3, Zero and CM1/CM3. Shows system clocks,
voltages, temperature, throttling and ARM/GPU-CMA memory utilization; QPU
scheduler, queues and VPM state; all 30 V3D performance-counter sources; and the
full V3D register map. The TUI adapts to the terminal width and has headless
snapshot, counter-dump, register-dump and CSV polling modes. Registers are read
through /dev/mem as root or through the read-only vc4mon sysfs module when
unprivileged; performance-counter sampling still needs root.")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/rughinnit/vc4top")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE.txt")

set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_NAME "vc4top")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "rughinnit <rughinnit@users.noreply.github.com>")
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/rughinnit/vc4top")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libc6 (>= 2.38), libstdc++6 (>= 13), libgcc-s1 (>= 3.0)")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "raspi-config")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")

# vc4top maps /dev/mem and must be started with sudo/root. It is deliberately
# not installed setuid: root access is explicit, not implicit.
include(CPack)
