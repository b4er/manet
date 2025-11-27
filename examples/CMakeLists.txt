add_executable(binance-sbe-example config.cc main.cc)

target_link_libraries(binance-sbe-example PRIVATE manet::manet)
