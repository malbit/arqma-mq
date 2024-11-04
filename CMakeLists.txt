cmake_minimum_required(VERSION 3.10)

project(libarqmamq CXX)

include(GNUInstallDirs)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

add_library(arqmamq
    arqmamq/arqmamq.cpp
    arqmamq/bt_serialize.cpp
)

install(
    FILES arqmamq/batch.h
          arqmamq/bt_serialize.h
          arqmamq/hex.h
          arqmamq/arqmamq.h
          arqmamq/string_view.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/arqmamq
)

# We require cppzmq >= 4.3, and versions before 4.3.2 have some security issues, so try to find
# >=4.3.2 on the system and, if we don't (or found an old one) build a bundled one.
include(FindPkgConfig)
pkg_search_module(libzmq QUIET libzmq>=4.3.2)
if(NOT libzmq_FOUND)
    message(STATUS "libzmq >= 4.3.2 not found, building bundled 4.3.2")
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/local-libzmq")
    include(LocalLibzmq)
endif()

set(CPPZMQ_BUILD_TESTS OFF CACHE BOOL "Disable cppzmq tests")
add_subdirectory(cppzmq EXCLUDE_FROM_ALL)

target_include_directories(arqmamq
    PUBLIC
        $<INSTALL_INTERFACE:>
        $<INSTALL_INTERFACE:mapbox-variant/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/mapbox-variant/include>
)

target_compile_options(arqmamq PRIVATE -Wall -Wextra -Werror)
set_target_properties(arqmamq PROPERTIES
    CXX_STANDARD 14
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)

target_link_libraries(arqmamq PUBLIC cppzmq)

if(NOT SODIUM_LIBRARIES)
    pkg_search_module(SODIUM REQUIRED libsodium)
endif()

# Really want to just use SODIUM_LINK_LIBRARIES here, but that isn't in older cmake:
find_library(libsodium_link_libs NAMES ${SODIUM_LIBRARIES} PATHS ${SODIUM_LIBRARY_DIRS})
target_link_libraries(arqmamq PRIVATE ${libsodium_link_libs})

target_include_directories(arqmamq PRIVATE ${SODIUM_INCLUDE_DIRS})

add_library(arqmamq::arqmamq ALIAS arqmamq)

export(
    TARGETS arqmamq
    NAMESPACE arqmamq::
    FILE arqmamqTargets.cmake
)
install(
    TARGETS arqmamq
    EXPORT arqmamqConfig
    DESTINATION lib
)

if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
  set(arqmamq_IS_TOPLEVEL_PROJECT TRUE)
else()
  set(arqmamq_IS_TOPLEVEL_PROJECT FALSE)
endif()

option(ARQMAMQ_BUILD_TESTS "Building and perform arqmamq tests" ${arqmamq_IS_TOPLEVEL_PROJECT})
if(ARQMAMQ_BUILD_TESTS)
    add_subdirectory(tests)
endif()

