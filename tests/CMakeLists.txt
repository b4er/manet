find_package(doctest REQUIRED)

# --- unit tests
file(GLOB_RECURSE UNIT_TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/unit/*.cc)

add_executable(unit-tests ${UNIT_TEST_SOURCES})
target_link_libraries(unit-tests PRIVATE manet::manet doctest::doctest)
add_test(NAME unit-tests COMMAND unit-tests)

set_tests_properties(unit-tests PROPERTIES LABELS unit)

# --- Integration tests

# start server
add_test(NAME server-start COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/server.py
                                   --start
)
set_tests_properties(server-start PROPERTIES FIXTURES_SETUP server)

set(SSL_CERT_FILE ${CMAKE_CURRENT_SOURCE_DIR}/cert/test-ca.pem)

file(GLOB_RECURSE INTEGRATION_TEST_SOURCES
     ${CMAKE_CURRENT_SOURCE_DIR}/integration/*.cc
)

add_executable(integration-tests ${INTEGRATION_TEST_SOURCES})
target_link_libraries(integration-tests PRIVATE manet::manet doctest::doctest)
add_test(NAME integration-tests COMMAND integration-tests)

set_tests_properties(
  integration-tests PROPERTIES LABELS integration TIMEOUT 30 ENVIRONMENT
                               "SSL_CERT_FILE=${SSL_CERT_FILE}"
)

# stop server fixture
add_test(NAME server-stop COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/server.py --stop)
set_tests_properties(server-stop PROPERTIES FIXTURES_CLEANUP server)

# --- coverage
if(MANET_ENABLE_COVERAGE)
  message(STATUS "Coverage enabled for unit-tests")

  find_program(LCOV_PATH lcov)
  find_program(GENHTML_PATH genhtml)

  # tests need coverage flags too:
  target_compile_options(unit-tests PRIVATE -O0 -g --coverage)
  target_link_options(unit-tests PRIVATE --coverage)

  target_compile_options(integration-tests PRIVATE -O0 -g --coverage)
  target_link_options(integration-tests PRIVATE --coverage)

  if(LCOV_PATH AND GENHTML_PATH)
    add_custom_target(
      coverage
      # run just unit-tests
      COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
      # capture coverage
      COMMAND
        ${LCOV_PATH} --ignore-errors inconsistent,inconsistent --rc
        geninfo_unexecuted_blocks=1 --capture --all --directory
        ${CMAKE_BINARY_DIR} --base-directory ${CMAKE_SOURCE_DIR} --output-file
        coverage.info --no-external
      # filter
      COMMAND ${LCOV_PATH} --ignore-errors inconsistent,inconsistent --extract
              coverage.info --output-file coverage.filtered.info
      COMMAND
        ${LCOV_PATH} --ignore-errors inconsistent,inconsistent --remove
        coverage.filtered.info "*/tests/*" --output-file coverage.filtered.info
      # html report
      COMMAND ${GENHTML_PATH} coverage.filtered.info --prefix
              ${CMAKE_SOURCE_DIR} --output-directory coverage_html
      WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )

    add_dependencies(coverage unit-tests)
  else()
    message(WARNING "lcov/genhtml not found; coverage target disabled")
  endif()
endif()
