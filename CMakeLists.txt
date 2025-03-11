
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
  foreach(lang C CXX)
    if(NOT DEFINED CMAKE_${lang}_COMPILER_LAUNCHER AND NOT CMAKE_${lang}_COMPILER MATCHES ".*/ccache")
      message(STATUS "Enabling ccache for ${lang}")
      set(CMAKE_${lang}_COMPILER_LAUNCHER ${CCACHE_PROGRAM} CACHE STRING "")
    endif()
  endforeach()
endif()

cmake_minimum_required(VERSION 3.10)

set(CMAKE_OSX_DEPLOYMENT_TARGET 10.12 CACHE STRING "macOS deployment target")

project(libarqmamq
  VERSION 1.0.0
  LANGUAGES CXX C)

include(GNUInstallDirs)

message(STATUS "arqmamq v${PROJECT_VERSION}")

set(ARQMAMQ_LIBVERSION 0)

if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
  set(arqmamq_IS_TOPLEVEL_PROJECT TRUE)
else()
  set(arqmamq_IS_TOPLEVEL_PROJECT FALSE)
endif()

option(BUILD_SHARED_LIBS "Build shared libraries instead of static ones" ON)
set(arqmamq_INSTALL_DEFAULT OFF)
if(BUILD_SHARED_LIBS OR arqmamq_IS_TOPLEVEL_PROJECT)
  set(arqmamq_INSTALL_DEFAULT ON)
endif()
option(ARQMAMQ_INSTALL "Add ArqmaMQ libraries and headers to cmake install target. Defaults to ON if BUILD_SHARED_LIBS is enabled or we are the top-level project. OFF for a static subdirectory build" ${arqmamq_INSTALL_DEFAULT})
option(ARQMAMQ_INSTALL_CPPZMQ "Install cppzmq header with arqmamq/ headers (requires ARQMAMQ_INSTALL)" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

configure_file(arqmamq/version.h.in arqmamq/version.h @ONLY)
configure_file(libarqmamq.pc.in libarqmamq.pc @ONLY)

add_library(arqmamq
    arqmamq/address.cpp
    arqmamq/auth.cpp
    arqmamq/bt_serialize.cpp
    arqmamq/connections.cpp
    arqmamq/jobs.cpp
    arqmamq/arqmamq.cpp
    arqmamq/proxy.cpp
    arqmamq/worker.cpp
)
set_target_properties(arqmamq PROPERTIES SOVERSION ${ARQMAMQ_LIBVERSION})

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
target_link_libraries(arqmamq PRIVATE Threads::Threads)

set(arqmamq_build_static_libzmq OFF)
if(TARGET libzmq)
  target_link_libraries(arqmamq PUBLIC libzmq)
elseif(BUILD_SHARED_LIBS)
  include(FindPkgConfig)
  pkg_check_modules(libzmq libzmq>=4.3 IMPORTED_TARGET)

  if(libzmq_FOUND)
    get_property(zmq_inc TARGET PkgConfig::libzmq PROPERTY INTERFACE_INCLUDE_DIRECTORIES)
    list(FIND zmq_inc "-isystem" broken_isystem)
    if(NOT broken_isystem EQUAL -1)
      list(REMOVE_AT zmq_inc ${broken_isystem})
      set_property(TARGET PkgConfig::libzmq PROPERTY INTERFACE_INCLUDE_DIRECTORIES ${zmq_inc})
    endif()

    target_link_libraries(arqmamq PUBLIC PkgConfig::libzmq)
  else()
    set(arqmamq_build_static_libzmq ON)
  endif()
else()
  set(arqmamq_build_static_libzmq ON)
endif()

if(arqmamq_build_static_libzmq)
  message(STATUS "libzmq >= 4.3 not found or static build request. Building bundled")
  list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/local-libzmq")
  include(LocalLibzmq)
  target_link_libraries(arqmamq PUBLIC libzmq_vendor)
endif()

target_include_directories(arqmamq
    PUBLIC
        $<INSTALL_INTERFACE:>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/cppzmq>
)

target_compile_options(arqmamq PRIVATE -Wall -Wextra -Werror)
set_target_properties(arqmamq PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    POSITION_INDEPENDENT_CODE ON
)

function(link_dep_libs target linktype libdirs)
  foreach(lib ${ARGN})
    find_library(link_lib-${lib} NAMES ${lib} PATHS ${libdirs})
    if(link_lib-${lib})
      target_link_libraries(${target} ${linktype} ${link_lib-${lib}})
    endif()
  endforeach()
endfunction()

if(TARGET sodium)
  target_link_libraries(arqmamq PUBLIC sodium)
  if(arqmamq_build_static_libzmq)
    target_link_libraries(libzmq_vendor INTERFACE sodium)
  endif()
else()
  include(FindPkgConfig)
  pkg_check_modules(sodium REQUIRED libsodium IMPORTED_TARGET)

  if(BUILD_SHARED_LIBS)
    target_link_libraries(arqmamq PUBLIC PkgConfig::sodium)
    if(arqmamq_build_static_libzmq)
      target_link_libraries(libzmq_vendor INTERFACE PkgConfig::sodium)
    endif()
  else()
    link_dep_libs(arqmamq PUBLIC "${sodium_STATIC_LIBRARY_DIRS}" ${sodium_STATIC_LIBRARIES})
    target_include_directories(arqmamq PUBLIC ${sodium_STATIC_INCLUDE_DIRS})
    if(arqmamq_build_static_libzmq)
      link_dep_libs(libzmq_vendor INTERFACE "${sodium_STATIC_LIBRARY_DIRS}" ${sodium_STATIC_LIBRARIES})
      target_link_libraries(libzmq_vendor INTERFACE ${sodium_STATIC_INCLUDE_DIRS})
    endif()
  endif()
endif()

add_library(arqmamq::arqmamq ALIAS arqmamq)

export(
    TARGETS arqmamq
    NAMESPACE arqmamq::
    FILE arqmamqTargets.cmake
)

if(ARQMAMQ_INSTALL)
  install(
    TARGETS arqmamq
    EXPORT arqmamqConfig
    DESTINATION ${CMAKE_INSTALL_LIBDIR}
  )

  install(
    FILES
        arqmamq/address.h
        arqmamq/auth.h
        arqmamq/base32z.h
        arqmamq/base64.h
        arqmamq/batch.h
        arqmamq/bt_serialize.h
        arqmamq/bt_value.h
        arqmamq/byte_type.h
        arqmamq/connections.h
        arqmamq/hex.h
        arqmamq/arqmamq.h
        arqmamq/message.h
        arqmamq/variant.h
        ${CMAKE_CURRENT_BINARY_DIR}/arqmamq/version.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/arqmamq
  )

  if(ARQMAMQ_INSTALL_CPPZMQ)
    install(
      FILES cppzmq/zmq.hpp
      DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/arqmamq
    )
  endif()

  install(
    FILES ${CMAKE_CURRENT_BINARY_DIR}/libarqmamq.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
  )
endif()
