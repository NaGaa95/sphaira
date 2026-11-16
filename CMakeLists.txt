cmake_minimum_required(VERSION 3.13)

if (NOT DEFINED ENV{DEVKITPRO})
    message(FATAL_ERROR "DEVKITPRO is not defined!")
endif()

option(SWITCH2 "build for the Nintendo Switch 2 (needs the ounce-devkit)" OFF)

if (NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if (SWITCH2)
        if (EXISTS $ENV{DEVKITPRO}/libounce/cmake/Switch2.cmake)
            set(CMAKE_TOOLCHAIN_FILE $ENV{DEVKITPRO}/libounce/cmake/Switch2.cmake)
        else()
            message(FATAL_ERROR "the Switch 2 devkit is not installed in $ENV{DEVKITPRO}, see https://github.com/ouncebrew/ounce-devkit")
        endif()
    elseif (EXISTS $ENV{DEVKITPRO}/cmake/Switch.cmake)
        set(CMAKE_TOOLCHAIN_FILE $ENV{DEVKITPRO}/cmake/Switch.cmake)
    else()
        message(FATAL_ERROR "please run 'sudo pacman -S switch-cmake`")
    endif()
endif()

project(sphaira LANGUAGES C CXX)

# also covers a build configured with the Switch 2 toolchain directly.
if (NINTENDO_SWITCH2)
    set(SWITCH2 ON)
    message(STATUS "building for Switch 2 with ${CMAKE_TOOLCHAIN_FILE}")
elseif (SWITCH2)
    message(FATAL_ERROR "SWITCH2 is set, but ${CMAKE_TOOLCHAIN_FILE} is not the Switch 2 toolchain. Use a new build directory.")
endif()

# enable setting cmake options via set()
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

# export compile commands
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# enable LTO (only in release builds)
if (LTO)
    if (CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
        include(CheckIPOSupported)
        check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
        if (ipo_supported)
            message(STATUS "IPO / LTO enabled for ALL targets")
            cmake_policy(SET CMP0069 NEW)
            set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)
            set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
        else()
            message(STATUS "IPO / LTO not supported: <${ipo_error}>")
        endif()
    else()
        message(STATUS "IPO / LTO not enabled in debug build")
    endif()
endif()

function(dkp_fatal_if_not_found var package)
    if (DEFINED ${var}_NOT_FOUND OR DEFINED ${var}-NOTFOUND)
        message(FATAL_ERROR "${package} not found, please run pacman -S switch-${package}")
    endif()
endfunction(dkp_fatal_if_not_found var package)

# disable exceptions and rtti in order to shrink final binary size.
add_compile_options(
  "$<$<COMPILE_LANGUAGE:C>:-fno-exceptions>"
  "$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>"
  "$<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>"
)

# hbl is only embedded in forwarders, which Switch 2 cannot install.
if (NOT SWITCH2)
    add_subdirectory(hbl)
endif()
add_subdirectory(sphaira)
