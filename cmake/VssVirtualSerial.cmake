# SPDX-License-Identifier: GPL-3.0-or-later
#
# Resolve Boost headers and the Boost.VirtualSerial checkout.
#
# Boost.VirtualSerial is not part of a Boost release yet, so it is consumed
# as a custom header-only target.  The resolution order is:
#
#   1. an already defined Boost::virtual_serial target (superproject build)
#   2. VSS_VIRTUAL_SERIAL_DIR, a checkout of the library
#   3. FetchContent of a release tag from the public repository, unless
#      VSS_FETCH_VIRTUAL_SERIAL is OFF (it is ON by default)
#   4. find_package(boost_virtual_serial CONFIG)
#
# To build against a local checkout, set VSS_VIRTUAL_SERIAL_DIR, for example
# in a CMakeUserPresets.json, which Git ignores.

include_guard(GLOBAL)

set(VSS_BOOST_ROOT "" CACHE PATH
  "Root of a Boost source tree (the directory containing boost/asio.hpp)")
set(VSS_VIRTUAL_SERIAL_DIR "" CACHE PATH
  "Path to a Boost.VirtualSerial checkout (the directory containing include/boost/virtual_serial.hpp)")
option(VSS_FETCH_VIRTUAL_SERIAL
  "Fetch Boost.VirtualSerial from git when no local checkout is configured" ON)
set(VSS_VIRTUAL_SERIAL_GIT_REPO
  "https://github.com/iainchesworthlabs/virtual-serial.git" CACHE STRING
  "Git repository used by VSS_FETCH_VIRTUAL_SERIAL")
set(VSS_VIRTUAL_SERIAL_GIT_TAG "v0.1.0-beta.2" CACHE STRING
  "Git tag or branch used by VSS_FETCH_VIRTUAL_SERIAL")

# ---------------------------------------------------------------------------
# Boost headers
# ---------------------------------------------------------------------------

if(NOT VSS_BOOST_ROOT
   AND VSS_VIRTUAL_SERIAL_DIR
   AND EXISTS "${VSS_VIRTUAL_SERIAL_DIR}/.cache/boost-root/boost/asio.hpp")
  set(VSS_BOOST_ROOT "${VSS_VIRTUAL_SERIAL_DIR}/.cache/boost-root")
endif()

find_path(VSS_BOOST_INCLUDE_DIR boost/asio.hpp
  HINTS
    "${VSS_BOOST_ROOT}"
    "$ENV{BOOST_ROOT}"
    "$ENV{BOOST_INCLUDEDIR}"
    "${VSS_VIRTUAL_SERIAL_DIR}/.cache/boost-root"
  PATH_SUFFIXES include)

if(NOT VSS_BOOST_INCLUDE_DIR)
  find_package(Boost 1.88 REQUIRED)
  if(TARGET Boost::headers)
    set(VSS_BOOST_TARGET Boost::headers)
  elseif(TARGET Boost::boost)
    set(VSS_BOOST_TARGET Boost::boost)
  else()
    message(FATAL_ERROR
      "Boost headers were not found. Set VSS_BOOST_ROOT (or BOOST_ROOT) to a "
      "Boost tree, or install Boost >= 1.88.")
  endif()
else()
  add_library(Vss::Boost INTERFACE IMPORTED GLOBAL)
  set_target_properties(Vss::Boost PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${VSS_BOOST_INCLUDE_DIR}"
    # Everything here is header only; never auto-link a compiled Boost.
    INTERFACE_COMPILE_DEFINITIONS "BOOST_ALL_NO_LIB")
  set(VSS_BOOST_TARGET Vss::Boost)
endif()

# ---------------------------------------------------------------------------
# Boost.VirtualSerial
# ---------------------------------------------------------------------------

if(TARGET Boost::virtual_serial)
  add_library(Vss::VirtualSerial ALIAS Boost::virtual_serial)
  message(STATUS "Vss: using the Boost::virtual_serial target already in the build")
elseif(TARGET boost_virtual_serial)
  add_library(Vss::VirtualSerial ALIAS boost_virtual_serial)
  message(STATUS "Vss: using the boost_virtual_serial target already in the build")
elseif(VSS_VIRTUAL_SERIAL_DIR
       AND EXISTS "${VSS_VIRTUAL_SERIAL_DIR}/include/boost/virtual_serial.hpp")
  add_library(Vss::VirtualSerial INTERFACE IMPORTED GLOBAL)
  set_target_properties(Vss::VirtualSerial PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${VSS_VIRTUAL_SERIAL_DIR}/include"
    INTERFACE_LINK_LIBRARIES "${VSS_BOOST_TARGET}"
    INTERFACE_COMPILE_FEATURES cxx_std_14)
elseif(VSS_VIRTUAL_SERIAL_DIR)
  message(FATAL_ERROR
    "VSS_VIRTUAL_SERIAL_DIR is set to '${VSS_VIRTUAL_SERIAL_DIR}', which does "
    "not contain include/boost/virtual_serial.hpp.")
elseif(VSS_FETCH_VIRTUAL_SERIAL)
  include(FetchContent)
  # SOURCE_SUBDIR names a directory that does not exist, so the library is
  # downloaded but its own build is not added: only its headers are used.
  FetchContent_Declare(boost_virtual_serial
    GIT_REPOSITORY "${VSS_VIRTUAL_SERIAL_GIT_REPO}"
    GIT_TAG "${VSS_VIRTUAL_SERIAL_GIT_TAG}"
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR vss-headers-only)
  FetchContent_MakeAvailable(boost_virtual_serial)
  FetchContent_GetProperties(boost_virtual_serial)
  set(VSS_VIRTUAL_SERIAL_DIR "${boost_virtual_serial_SOURCE_DIR}")
  add_library(Vss::VirtualSerial INTERFACE IMPORTED GLOBAL)
  set_target_properties(Vss::VirtualSerial PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${VSS_VIRTUAL_SERIAL_DIR}/include"
    INTERFACE_LINK_LIBRARIES "${VSS_BOOST_TARGET}"
    INTERFACE_COMPILE_FEATURES cxx_std_14)
  message(STATUS "Vss: fetched Boost.VirtualSerial ${VSS_VIRTUAL_SERIAL_GIT_TAG} "
    "from ${VSS_VIRTUAL_SERIAL_GIT_REPO}")
else()
  find_package(boost_virtual_serial CONFIG QUIET)
  if(TARGET Boost::virtual_serial)
    add_library(Vss::VirtualSerial ALIAS Boost::virtual_serial)
  else()
    message(FATAL_ERROR
      "Boost.VirtualSerial was not found.\n"
      "  Set VSS_VIRTUAL_SERIAL_DIR to a checkout of the library, for example\n"
      "    -DVSS_VIRTUAL_SERIAL_DIR=/path/to/virtual-serial\n"
      "  or configure with -DVSS_FETCH_VIRTUAL_SERIAL=ON to fetch a release.")
  endif()
endif()

set(VSS_VIRTUAL_SERIAL_TARGET Vss::VirtualSerial)
message(STATUS "Vss: Boost.VirtualSerial from ${VSS_VIRTUAL_SERIAL_DIR}")
