cmake_minimum_required(VERSION 3.20)

project(manet CXX)

option(MANET_BUILD_TESTING "Build tests" ON)
option(MANET_BUILD_EXAMPLES "Build examples" OFF)

option(MANET_ENABLE_COVERAGE "Build with coverage instrumentation" OFF)
option(MANET_USE_FSTACK "Enable F-Stack backend" OFF)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE
      Debug
      CACHE STRING "Build type" FORCE
  )
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- dependencies
find_package(OpenSSL REQUIRED)

if(USE_FSTACK)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(FSTACK REQUIRED libdpdk)
  pkg_check_modules(DPDK REQUIRED fstack)

  add_compile_definitions(USE_FSTACK)
endif()

# lib headers and sources:
set(MANET_PUBLIC_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)

file(GLOB_RECURSE MANET_SRC_FILES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cc)

if(MANET_USE_FSTACK)
  list(REMOVE_ITEM MANET_SRC_FILES ${CMAKE_SOURCE_DIR}/src/net/backend/posix.cc)
else()
  list(REMOVE_ITEM MANET_SRC_FILES
       ${CMAKE_SOURCE_DIR}/src/net/backend/f-stack.cc
  )
endif()

# --- manet library
add_library(manet ${MANET_SRC_FILES})
add_library(manet::manet ALIAS manet)

target_include_directories(
  manet PUBLIC $<BUILD_INTERFACE:${MANET_PUBLIC_INCLUDE_DIR}>
               $<INSTALL_INTERFACE:include>
)

if(MANET_USE_FSTACK)
  target_include_directories(
    manet PRIVATE ${DPDK_INCLUDE_DIRS} ${FSTACK_INCLUDE_DIRS}
  )
  target_link_directories(
    manet PRIVATE ${DPDK_LIBRARY_DIRS} ${FSTACK_LIBRARY_DIRS}
  )
endif()

target_compile_definitions(manet PUBLIC $<$<CONFIG:Debug>:DEBUG>)

if(MANET_ENABLE_COVERAGE)
  target_compile_options(manet PRIVATE -O0 -g --coverage)
endif()

target_link_options(
  manet
  PRIVATE
  $<$<AND:$<CONFIG:Debug>,$<NOT:$<BOOL:${MANET_ENABLE_COVERAGE}>>>:
  -fsanitize=address,undefined>
  $<$<AND:$<CONFIG:Release>,$<NOT:$<BOOL:${MANET_ENABLE_COVERAGE}>>>:
  -O3>
  $<$<BOOL:${MANET_ENABLE_COVERAGE}>:
  --coverage>
)

target_link_libraries(manet PUBLIC OpenSSL::SSL)

if(MANET_USE_FSTACK)
  target_link_libraries(
    manet PUBLIC ${DPDK_LIBRARIES} ${FSTACK_LIBRARIES} rte_pmd_bond
  )
endif()

# install

install(
  TARGETS manet
  EXPORT manetTargets
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
)

install(DIRECTORY include/ DESTINATION include)

# --- tests and examples

if(MANET_BUILD_TESTING)
  include(CTest)
  enable_testing()
  add_subdirectory(tests)
endif()

# if(MANET_BUILD_EXAMPLES) add_subdirectory(examples) endif()
