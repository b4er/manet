# SBE codegen (configure phase):

find_package(sbepp REQUIRED)
find_program(SBEPPC sbeppc REQUIRED)

set(SCHEMA_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/schema/include")
file(MAKE_DIRECTORY "${SCHEMA_OUT_DIR}")

set(binance_sbe_schema "${CMAKE_CURRENT_SOURCE_DIR}/binance-spot-sbe.xml")

set_property(
  DIRECTORY
  APPEND
  PROPERTY CMAKE_CONFIGURE_DEPENDS "${binance_sbe_schema}"
)

execute_process(
  COMMAND "${SBEPPC}" --schema-name binance_sbe --output-dir "${SCHEMA_OUT_DIR}"
          "${binance_sbe_schema}"
  WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  RESULT_VARIABLE SBEPPC_RESULT
)

if(NOT SBEPPC_RESULT EQUAL 0)
  message(FATAL_ERROR "sbeppc failed with exit code ${SBEPPC_RESULT}")
endif()

file(GLOB_RECURSE BINANCE_SBE_HEADERS "${SCHEMA_OUT_DIR}/binance_sbe.hpp"
     "${SCHEMA_OUT_DIR}/binance_sbe/**/*.hpp"
)

add_library(binance_sbe INTERFACE)

target_sources(binance_sbe INTERFACE ${BINANCE_SBE_HEADERS})
target_include_directories(binance_sbe INTERFACE "${SCHEMA_OUT_DIR}")

# example

add_executable(binance-sbe ${BINANCE_SBE_HEADERS} config.cc main.cc)
target_link_libraries(binance-sbe PRIVATE manet::manet sbepp::sbepp binance_sbe)
